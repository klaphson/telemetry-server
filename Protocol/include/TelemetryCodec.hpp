#ifndef TELEMETRY_CODEC_HPP
#define TELEMETRY_CODEC_HPP

#include "TelemetryRecord.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace telemetry::protocol
{

inline constexpr std::uint32_t kMagic = 0x544C5259U; // "TLRY"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kFrameSize = 32;

using Frame = std::array<std::byte, kFrameSize>;

enum class DecodeError {
    None,
    InvalidSize,
    InvalidMagic,
    UnsupportedVersion,
    InvalidDeclaredFrameSize
};

struct DecodeResult {
    TelemetryRecord record{};
    DecodeError error{DecodeError::None};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == DecodeError::None;
    }
};

[[nodiscard]] Frame encode(const TelemetryRecord &record);

[[nodiscard]] DecodeResult decode(std::span<const std::byte> frame);

[[nodiscard]] const char* toString(DecodeError error) noexcept;

} // namespace telemetry::protocol

#endif // TELEMETRY_CODEC_HPP