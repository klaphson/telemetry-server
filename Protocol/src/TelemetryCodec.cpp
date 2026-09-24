#include "TelemetryCodec.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace telemetry::protocol
{

namespace
{

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kFrameSizeOffset = 6;
constexpr std::size_t kSensorIdOffset = 8;
constexpr std::size_t kMetricIdOffset = 12;
constexpr std::size_t kTimestampOffset = 16;
constexpr std::size_t kValueOffset = 24;

static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559);

void writeU16(Frame &frame, std::size_t offset, std::uint16_t value)
{
    frame[offset] = std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)};

    frame[offset + 1] = std::byte{static_cast<unsigned char>(value & 0xFFU)};
}

void writeU32(Frame &frame, std::size_t offset, std::uint32_t value)
{
    frame[offset] = std::byte{static_cast<unsigned char>((value >> 24U) & 0xFFU)};

    frame[offset + 1] = std::byte{static_cast<unsigned char>((value >> 16U) & 0xFFU)};

    frame[offset + 2] = std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)};

    frame[offset + 3] = std::byte{static_cast<unsigned char>(value & 0xFFU)};
}

void writeU64(Frame &frame, std::size_t offset, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned int>((7U - static_cast<unsigned int>(index)) * 8U);

        frame[offset + index] = std::byte{static_cast<unsigned char>((value >> shift) & 0xFFULL)};
    }
}

std::uint16_t readU16(std::span<const std::byte> frame, std::size_t offset)
{
    const auto byte0 = std::to_integer<std::uint16_t>(frame[offset]);

    const auto byte1 = std::to_integer<std::uint16_t>(frame[offset + 1]);

    return static_cast<std::uint16_t>((byte0 << 8U) | byte1);
}

std::uint32_t readU32(std::span<const std::byte> frame, std::size_t offset)
{
    const auto byte0 = std::to_integer<std::uint32_t>(frame[offset]);

    const auto byte1 = std::to_integer<std::uint32_t>(frame[offset + 1]);

    const auto byte2 = std::to_integer<std::uint32_t>(frame[offset + 2]);

    const auto byte3 = std::to_integer<std::uint32_t>(frame[offset + 3]);

    return (byte0 << 24U) | (byte1 << 16U) | (byte2 << 8U) | byte3;
}

std::uint64_t readU64(std::span<const std::byte> frame, std::size_t offset)
{
    std::uint64_t value = 0;

    for (std::size_t index = 0; index < 8; ++index) {
        value <<= 8U;

        value |= std::to_integer<std::uint64_t>(frame[offset + index]);
    }

    return value;
}

} // namespace

Frame encode(const TelemetryRecord &record)
{
    Frame frame{};

    writeU32(frame, kMagicOffset, kMagic);

    writeU16(frame, kVersionOffset, kVersion);

    writeU16(frame, kFrameSizeOffset, static_cast<std::uint16_t>(kFrameSize));

    writeU32(frame, kSensorIdOffset, record.sensorId);

    writeU32(frame, kMetricIdOffset, record.metricId);

    writeU64(frame, kTimestampOffset, record.timestampNs);

    const std::uint64_t valueBits = std::bit_cast<std::uint64_t>(record.value);

    writeU64(frame, kValueOffset, valueBits);

    return frame;
}

DecodeResult decode(std::span<const std::byte> frame)
{
    if (frame.size() != kFrameSize) {
        return DecodeResult{{}, DecodeError::InvalidSize};
    }

    if (readU32(frame, kMagicOffset) != kMagic) {
        return DecodeResult{{}, DecodeError::InvalidMagic};
    }

    if (readU16(frame, kVersionOffset) != kVersion) {
        return DecodeResult{{}, DecodeError::UnsupportedVersion};
    }

    if (readU16(frame, kFrameSizeOffset) != static_cast<std::uint16_t>(kFrameSize)) {

        return DecodeResult{{}, DecodeError::InvalidDeclaredFrameSize};
    }

    TelemetryRecord record{};

    record.sensorId = readU32(frame, kSensorIdOffset);

    record.metricId = readU32(frame, kMetricIdOffset);

    record.timestampNs = readU64(frame, kTimestampOffset);

    const std::uint64_t valueBits = readU64(frame, kValueOffset);

    record.value = std::bit_cast<double>(valueBits);

    return DecodeResult{record, DecodeError::None};
}

const char* toString(DecodeError error) noexcept
{
    switch (error) {
    case DecodeError::None:
        return "none";

    case DecodeError::InvalidSize:
        return "invalid frame size";

    case DecodeError::InvalidMagic:
        return "invalid magic";

    case DecodeError::UnsupportedVersion:
        return "unsupported version";

    case DecodeError::InvalidDeclaredFrameSize:
        return "invalid declared frame size";
    }

    return "unknown decode error";
}

} // namespace telemetry::protocol
