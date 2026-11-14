#ifndef APP_CONFIG_HPP
#define APP_CONFIG_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace telemetry::config
{

enum class ReaderMode { Process, Thread };

struct AppConfig {
    ReaderMode readerMode = ReaderMode::Process;

    std::uint16_t port = 9000;

    std::string dataFilePath = "telemetry.bin";

    std::size_t maxSegmentBytes = 64 * 1024 * 1024;

    std::size_t syncEveryBytes = 1 * 1024 * 1024;

    bool quiet = false;
};

} // namespace telemetry::config

#endif