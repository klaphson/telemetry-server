#ifndef APP_CONFIG_HPP
#define APP_CONFIG_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace telemetry::config
{

struct AppConfig {
    std::uint16_t port = 9000;

    std::string dataFilePath = "telemetry.bin";

    std::size_t maxSegmentBytes = 64 * 1024 * 1024;

    std::size_t syncEveryBytes = 1 * 1024 * 1024;
};

} // namespace telemetry::config

#endif