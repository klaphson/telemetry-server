#include "TelemetryCodec.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
struct Options {
    std::uint64_t frames = 100000;
    std::size_t batchFrames = 128;
    std::uint16_t port = 19000;
};

bool parseNumber(std::string_view text, std::uint64_t &value)
{
    if (text.empty())
        return false;
    const auto [last, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && last == text.data() + text.size();
}

bool parseOptions(int argc, char* argv[], Options &o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view key{argv[i]};
        if (key == "--help") {
            std::cout << "usage: telemetry-load [--frames N] [--batch-frames N] [--port N]\n";
            std::exit(EXIT_SUCCESS);
        }
        if (key != "--frames" && key != "--batch-frames" && key != "--port")
            return false;
        if (++i == argc)
            return false;
        std::uint64_t value{};
        if (!parseNumber(argv[i], value))
            return false;
        if (key == "--frames")
            o.frames = value;
        if (key == "--batch-frames")
            o.batchFrames = static_cast<std::size_t>(value);
        if (key == "--port") {
            if (value > 65535)
                return false;
            o.port = static_cast<std::uint16_t>(value);
        }
    }
    return o.frames >= 1 && o.frames <= 1000000 && o.batchFrames >= 1 && o.batchFrames <= 4096 &&
           o.port >= 1;
}

bool sendAll(int fd, std::span<const std::byte> data)
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (n > 0)
            offset += static_cast<std::size_t>(n);
        else if (n == -1 && errno == EINTR)
            continue;
        else {
            if (n == -1)
                perror("send");
            else
                std::cerr << "send returned zero\n";
            return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char* argv[])
{
    Options o;
    if (!parseOptions(argc, argv, o)) {
        std::cerr << "usage: telemetry-load [--frames 1..1000000] [--batch-frames 1..4096] [--port "
                     "1..65535]\n";
        return EXIT_FAILURE;
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(o.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }

    using namespace telemetry::protocol;
    std::vector<std::byte> buffer(o.batchFrames * kFrameSize);
    const auto start = std::chrono::steady_clock::now();

    std::uint64_t sent = 0;
    while (sent < o.frames) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(o.batchFrames, o.frames - sent));
        for (std::size_t i = 0; i < count; ++i) {
            const Frame frame = encode(TelemetryRecord{
                .sensorId = 1, .metricId = 2, .timestampNs = sent + i + 1, .value = 1.0});
            std::copy(frame.begin(), frame.end(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(i * kFrameSize));
        }
        if (!sendAll(fd, {buffer.data(), count * kFrameSize})) {
            close(fd);
            return EXIT_FAILURE;
        }
        sent += count;
    }

    if (shutdown(fd, SHUT_WR) == -1) {
        perror("shutdown");
        close(fd);
        return EXIT_FAILURE;
    }
    close(fd);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << std::fixed << std::setprecision(3) << "sender_frames=" << sent << '\n'
              << "sender_bytes=" << sent * kFrameSize << '\n'
              << "sender_seconds=" << seconds << '\n'
              << "sender_frames_per_second=" << static_cast<double>(sent) / seconds << '\n';
    return EXIT_SUCCESS;
}
