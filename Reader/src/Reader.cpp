#include "Reader.hpp"
#include "TelemetryCodec.hpp"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

int Reader::run(int pipeReadFd) const
{
    std::cout << "[reader] pid=" << getpid() << '\n';

    std::cout << "[reader] reading binary telemetry from pipe\n";

    std::array<std::byte, bufferSize> buffer{};

    std::vector<std::byte> pending;
    std::size_t offset = 0;

    while (true) {
        const ssize_t bytesRead = read(pipeReadFd, buffer.data(), buffer.size());

        if (bytesRead > 0) {
            const auto count = static_cast<std::size_t>(bytesRead);
            if (!readFrames(std::span<const std::byte>(buffer.data(), count), pending, offset)) {
                return EXIT_FAILURE;
            }

            continue;
        }

        if (bytesRead == 0) {
            const std::size_t remaining = pending.size() - offset;

            if (remaining != 0) {
                std::cerr << "[reader] EOF with partial IPC frame: " << remaining << " bytes\n";

                return EXIT_FAILURE;
            }

            std::cout << "[reader] EOF\n";

            return EXIT_SUCCESS;
        }

        if (errno == EINTR) {
            continue;
        }

        perror("read");
        return EXIT_FAILURE;
    }
}

bool Reader::readFrames(std::span<const std::byte> bytes, std::vector<std::byte> &pending,
                        std::size_t &offset) const
{
    using namespace telemetry::protocol;

    pending.insert(pending.end(), bytes.begin(), bytes.end());

    while (pending.size() - offset >= kFrameSize) {

        const std::span<const std::byte> frame{pending.data() + offset, kFrameSize};

        const DecodeResult decoded = decode(frame);

        if (!decoded) {
            std::cerr << "[reader] invalid IPC frame: " << toString(decoded.error) << '\n';

            return false;
        }

        const TelemetryRecord &record = decoded.record;

        std::cout << "[reader] telemetry:" << " sensor=" << record.sensorId
                  << " metric=" << record.metricId << " timestamp_ns=" << record.timestampNs
                  << " value=" << record.value << '\n';

        offset += kFrameSize;
    }

    if (offset == pending.size()) {
        pending.clear();
        offset = 0;
    } else if (offset >= bufferSize) {
        using Difference = std::vector<std::byte>::difference_type;

        pending.erase(pending.begin(), pending.begin() + static_cast<Difference>(offset));

        offset = 0;
    }

    return true;
}
