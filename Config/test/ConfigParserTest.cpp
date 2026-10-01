#include "ConfigParser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace telemetry::config;

namespace
{

ParseStatus parseOptions(std::initializer_list<std::string> options, AppConfig &config,
                         std::ostream &errors)
{
    std::vector<std::string> arguments{"telemetry-server"};
    arguments.insert(arguments.end(), options.begin(), options.end());

    std::vector<char*> argv;
    for (auto &argument : arguments) {
        argv.push_back(argument.data());
    }
    const auto argc = static_cast<int>(argv.size());
    argv.push_back(nullptr);
    return parseArguments(argc, argv.data(), config, errors);
}

} // namespace

TEST_CASE("configuration uses defaults without arguments")
{
    char program[] = "telemetry-server";

    char* argv[] = {program};

    AppConfig config;
    std::ostringstream errors;

    const auto result = parseArguments(1, argv, config, errors);

    REQUIRE(result == ParseStatus::Success);

    REQUIRE(config.port == 9000);

    REQUIRE(config.dataFilePath == "telemetry.bin");

    REQUIRE(config.maxSegmentBytes == 64 * 1024 * 1024);

    REQUIRE(config.syncEveryBytes == 1 * 1024 * 1024);

    REQUIRE(errors.str().empty());
}

TEST_CASE("configuration parses runtime options")
{
    char program[] = "telemetry-server";

    char portOption[] = "--port";
    char port[] = "9100";

    char fileOption[] = "--data-file";

    char file[] = "/tmp/test.bin";

    char segmentOption[] = "--segment-bytes";

    char segment[] = "1048576";

    char syncOption[] = "--sync-bytes";

    char sync[] = "262144";

    char* argv[] = {program,       portOption, port,       fileOption, file,
                    segmentOption, segment,    syncOption, sync};

    AppConfig config;
    std::ostringstream errors;

    const auto status = parseArguments(9, argv, config, errors);

    REQUIRE(status == ParseStatus::Success);

    REQUIRE(config.port == 9100);

    REQUIRE(config.dataFilePath == "/tmp/test.bin");

    REQUIRE(config.maxSegmentBytes == 1048576);

    REQUIRE(config.syncEveryBytes == 262144);
}

TEST_CASE("configuration rejects ports outside the allowed range")
{
    for (const auto* value : {"0", "65536", "70000"}) {
        CAPTURE(value);
        AppConfig config;
        std::ostringstream errors;

        REQUIRE(parseOptions({"--port", value}, config, errors) == ParseStatus::Error);
        REQUIRE(errors.str().find("invalid port: " + std::string{value}) != std::string::npos);
    }
}

TEST_CASE("configuration rejects malformed unsigned numbers and overflow")
{
    struct Case {
        std::string option;
        std::string diagnostic;
        std::string overflow;
    };
    const std::string sizeOverflow = std::to_string(std::numeric_limits<std::size_t>::max()) + "0";
    const Case cases[] = {
        {"--port", "invalid port", "65536"},
        {"--segment-bytes", "invalid segment size", sizeOverflow},
        {"--sync-bytes", "invalid sync interval", sizeOverflow},
    };

    for (const auto &test : cases) {
        for (const auto &value : std::vector<std::string>{"abc", "", "-1", "+32", "32abc", "32.0",
                                                          " 32", "32 ", "0x20", test.overflow}) {
            CAPTURE(test.option, value);
            AppConfig config;
            std::ostringstream errors;

            REQUIRE(parseOptions({test.option, value}, config, errors) == ParseStatus::Error);
            REQUIRE(errors.str().find(test.diagnostic) != std::string::npos);
        }
    }
}

TEST_CASE("configuration rejects undersized and unaligned segments")
{
    struct Case {
        std::string value;
        std::string diagnostic;
    };
    const Case cases[] = {
        {"0", "segment size must be at least 32 bytes"},
        {"31", "segment size must be at least 32 bytes"},
        {"33", "segment size must be a multiple of 32 bytes"},
        {"63", "segment size must be a multiple of 32 bytes"},
    };

    for (const auto &test : cases) {
        CAPTURE(test.value);
        AppConfig config;
        std::ostringstream errors;

        // Disable syncing so only the segment-size constraint is exercised.
        REQUIRE(parseOptions({"--segment-bytes", test.value, "--sync-bytes", "0"}, config,
                             errors) == ParseStatus::Error);
        REQUIRE(errors.str().find(test.diagnostic) != std::string::npos);
    }
}

TEST_CASE("configuration rejects sync intervals larger than the segment")
{
    for (const auto* sync : {"33", "64"}) {
        for (const bool syncFirst : {false, true}) {
            CAPTURE(sync, syncFirst);
            AppConfig config;
            std::ostringstream errors;

            const auto status =
                syncFirst
                    ? parseOptions({"--sync-bytes", sync, "--segment-bytes", "32"}, config, errors)
                    : parseOptions({"--segment-bytes", "32", "--sync-bytes", sync}, config, errors);
            REQUIRE(status == ParseStatus::Error);
            REQUIRE(errors.str().find("sync interval cannot exceed segment size") !=
                    std::string::npos);
        }
    }

    SECTION("the default sync interval must also fit an explicitly smaller segment")
    {
        AppConfig config;
        std::ostringstream errors;

        REQUIRE(parseOptions({"--segment-bytes", "32"}, config, errors) == ParseStatus::Error);
        REQUIRE(errors.str().find("sync interval cannot exceed segment size") != std::string::npos);
    }
}

TEST_CASE("configuration rejects unknown options and positional arguments")
{
    for (const auto* argument : {"--unknown", "-p", "--port=9000", "unexpected"}) {
        CAPTURE(argument);
        AppConfig config;
        std::ostringstream errors;

        REQUIRE(parseOptions({"--port", "9100", argument}, config, errors) == ParseStatus::Error);
        REQUIRE(errors.str().find("unknown argument: " + std::string{argument}) !=
                std::string::npos);
    }
}

TEST_CASE("configuration rejects missing values for every value-taking option")
{
    for (const auto* option : {"--port", "--data-file", "--segment-bytes", "--sync-bytes"}) {
        CAPTURE(option);
        AppConfig config;
        std::ostringstream errors;

        REQUIRE(parseOptions({option}, config, errors) == ParseStatus::Error);
        REQUIRE(errors.str().find("missing value for " + std::string{option}) != std::string::npos);
    }
}

TEST_CASE("configuration rejects an empty data file path")
{
    AppConfig config;
    std::ostringstream errors;

    REQUIRE(parseOptions({"--data-file", ""}, config, errors) == ParseStatus::Error);
    REQUIRE(errors.str().find("invalid data file path") != std::string::npos);
}

TEST_CASE("configuration accepts port and storage boundaries")
{
    for (const auto* port : {"1", "65535"}) {
        // Sync thresholds may be zero, unaligned, or equal to the segment size.
        for (const auto* sync : {"0", "1", "31", "32"}) {
            for (const bool syncFirst : {false, true}) {
                CAPTURE(port, sync, syncFirst);
                AppConfig config;
                std::ostringstream errors;

                const auto status = syncFirst ? parseOptions({"--port", port, "--sync-bytes", sync,
                                                              "--segment-bytes", "32"},
                                                             config, errors)
                                              : parseOptions({"--port", port, "--segment-bytes",
                                                              "32", "--sync-bytes", sync},
                                                             config, errors);
                REQUIRE(status == ParseStatus::Success);
                REQUIRE(config.port == std::stoul(port));
                REQUIRE(config.maxSegmentBytes == 32);
                REQUIRE(config.syncEveryBytes == std::stoul(sync));
                REQUIRE(errors.str().empty());
            }
        }
    }
}

TEST_CASE("configuration recognizes both help options")
{
    for (const auto* option : {"-h", "--help"}) {
        CAPTURE(option);
        AppConfig config;
        std::ostringstream errors;

        REQUIRE(parseOptions({option}, config, errors) == ParseStatus::HelpRequested);
        REQUIRE(errors.str().empty());
    }
}
