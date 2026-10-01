#include "ConfigParser.hpp"
#include "TelemetryCodec.hpp"

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string_view>
#include <system_error>

namespace telemetry::config
{

namespace
{

template <std::unsigned_integral T> bool parseUnsigned(std::string_view text, T &result)
{
    if (text.empty()) {
        return false;
    }

    T parsed = 0;

    const char* begin = text.data();
    const char* end = begin + text.size();

    const auto [ptr, error] = std::from_chars(begin, end, parsed);

    if (error != std::errc{} || ptr != end) {
        return false;
    }

    result = parsed;

    return true;
}

bool validate(const AppConfig &config, std::ostream &errorOutput)
{
    using telemetry::protocol::kFrameSize;

    if (config.port == 0) {
        errorOutput << "port must be between 1 and 65535\n";

        return false;
    }

    if (config.dataFilePath.empty()) {
        errorOutput << "data file path cannot be empty\n";

        return false;
    }

    if (config.maxSegmentBytes < kFrameSize) {
        errorOutput << "segment size must be at least " << kFrameSize << " bytes\n";

        return false;
    }

    if (config.maxSegmentBytes % kFrameSize != 0) {
        errorOutput << "segment size must be a multiple of " << kFrameSize << " bytes\n";

        return false;
    }

    if (config.syncEveryBytes != 0 && config.syncEveryBytes > config.maxSegmentBytes) {

        errorOutput << "sync interval cannot exceed " << "segment size\n";

        return false;
    }

    return true;
}

} // namespace

ParseStatus parseArguments(int argc, char* const argv[], AppConfig &config,
                           std::ostream &errorOutput)
{
    for (int index = 1; index < argc; ++index) {

        const std::string_view argument{argv[index]};

        if (argument == "--help" || argument == "-h") {
            return ParseStatus::HelpRequested;
        }

        auto nextValue = [&]() -> std::optional<std::string_view> {
            if (index + 1 >= argc) {
                errorOutput << "missing value for " << argument << '\n';

                return std::nullopt;
            }

            ++index;

            return std::string_view{argv[index]};
        };

        if (argument == "--port") {
            const auto value = nextValue();

            if (!value) {
                return ParseStatus::Error;
            }

            if (!parseUnsigned(*value, config.port) || config.port == 0) {

                errorOutput << "invalid port: " << *value << '\n';

                return ParseStatus::Error;
            }

            continue;
        }

        if (argument == "--data-file") {
            const auto value = nextValue();

            if (!value || value->empty()) {
                errorOutput << "invalid data file path\n";

                return ParseStatus::Error;
            }

            config.dataFilePath = *value;

            continue;
        }

        if (argument == "--segment-bytes") {
            const auto value = nextValue();

            if (!value || !parseUnsigned(*value, config.maxSegmentBytes)) {

                errorOutput << "invalid segment size\n";

                return ParseStatus::Error;
            }

            continue;
        }

        if (argument == "--sync-bytes") {
            const auto value = nextValue();

            if (!value || !parseUnsigned(*value, config.syncEveryBytes)) {

                errorOutput << "invalid sync interval\n";

                return ParseStatus::Error;
            }

            continue;
        }

        errorOutput << "unknown argument: " << argument << '\n';

        return ParseStatus::Error;
    }

    if (!validate(config, errorOutput)) {
        return ParseStatus::Error;
    }

    return ParseStatus::Success;
}

void printUsage(std::ostream &output, std::string_view programName)
{
    output << "usage: " << programName << " [options]\n\n"
           << "options:\n"
           << "  --port PORT\n"
           << "  --data-file PATH\n"
           << "  --segment-bytes BYTES\n"
           << "  --sync-bytes BYTES\n"
           << "  -h, --help\n";
}

} // namespace telemetry::config