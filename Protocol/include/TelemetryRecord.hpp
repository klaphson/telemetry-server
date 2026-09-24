#ifndef TELEMETRY_RECORD_HPP
#define TELEMETRY_RECORD_HPP

#include <cstdint>

namespace telemetry::protocol
{

struct TelemetryRecord {
    std::uint32_t sensorId{};
    std::uint32_t metricId{};
    std::uint64_t timestampNs{};
    double value{};

    friend bool operator==(const TelemetryRecord &, const TelemetryRecord &) = default;
};

} // namespace telemetry::protocol

#endif // TELEMETRY_RECORD_HPP