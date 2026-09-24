#include "TelemetryCodec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

using namespace telemetry::protocol;

namespace
{

constexpr TelemetryRecord sampleRecord{0x01234567U, 0x89ABCDEFU, 0x0123456789ABCDEFULL, -123.5};

// Construct the wire fixture independently of encode(), so matching encoder and
// decoder bugs cannot hide a byte-order or field-offset regression.
constexpr std::array<std::byte, 32> wireFrame{
    std::byte{0x54}, std::byte{0x4C}, std::byte{0x52}, std::byte{0x59}, // "TLRY" magic
    std::byte{0x00}, std::byte{0x01},                                   // version 1
    std::byte{0x00}, std::byte{0x20},                                   // 32-byte frame
    std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67}, // sensor ID
    std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}, // metric ID
    std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67}, // timestamp
    std::byte{0x89}, std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF},
    std::byte{0xC0}, std::byte{0x5E}, std::byte{0xE0}, std::byte{0x00}, // -123.5
    std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
};

} // namespace

TEST_CASE("encode produces the expected big-endian wire frame", "[telemetry_codec]")
{
    REQUIRE(encode(sampleRecord) == wireFrame);
}

TEST_CASE("decode reads a known wire frame", "[telemetry_codec]")
{
    const auto result = decode(wireFrame);

    REQUIRE(result.error == DecodeError::None);
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.record == sampleRecord);
}

TEST_CASE("codec round trips integer boundaries", "[telemetry_codec]")
{
    constexpr auto maxId = std::numeric_limits<std::uint32_t>::max();
    constexpr auto maxTimestamp = std::numeric_limits<std::uint64_t>::max();
    const TelemetryRecord records[] = {
        {},
        {maxId, 0, 0, 1.0},
        {0, maxId, 0, -1.0},
        {0, 0, maxTimestamp, 0.5},
        {maxId, maxId, maxTimestamp, -0.5},
    };

    for (const auto &record : records) {
        CAPTURE(record.sensorId, record.metricId, record.timestampNs, record.value);
        const auto result = decode(encode(record));

        REQUIRE(result.error == DecodeError::None);
        REQUIRE(result.record == record);
    }
}

TEST_CASE("codec preserves floating-point bits", "[telemetry_codec]")
{
    const std::uint64_t patterns[] = {
        0x0000000000000000ULL, // positive zero
        0x8000000000000000ULL, // negative zero
        0x0000000000000001ULL, // smallest positive subnormal
        0x0010000000000000ULL, // smallest positive normal
        0x7FEFFFFFFFFFFFFFULL, // largest finite positive value
        0xFFEFFFFFFFFFFFFFULL, // largest finite negative magnitude
        0x7FF0000000000000ULL, // positive infinity
        0xFFF0000000000000ULL, // negative infinity
        0x7FF8000000001234ULL, // quiet NaN with a payload
        0xFFF8000000005678ULL, // negative quiet NaN with a payload
    };

    for (const auto bits : patterns) {
        CAPTURE(bits);
        auto record = sampleRecord;
        record.value = std::bit_cast<double>(bits);
        const auto frame = encode(record);

        for (std::size_t index = 0; index < 8; ++index) {
            CAPTURE(index);
            const auto expected = static_cast<unsigned char>(bits >> ((7 - index) * 8));
            REQUIRE(frame[24 + index] == std::byte{expected});
        }

        const auto result = decode(frame);
        REQUIRE(result.error == DecodeError::None);
        REQUIRE(std::bit_cast<std::uint64_t>(result.record.value) == bits);
    }
}

TEST_CASE("decode rejects every truncated frame", "[telemetry_codec]")
{
    REQUIRE(decode({}).error == DecodeError::InvalidSize);

    for (std::size_t size = 0; size < wireFrame.size(); ++size) {
        CAPTURE(size);
        const auto result = decode(std::span<const std::byte>{wireFrame}.first(size));

        REQUIRE(result.error == DecodeError::InvalidSize);
        REQUIRE_FALSE(static_cast<bool>(result));
    }
}

TEST_CASE("decode rejects trailing bytes and concatenated frames", "[telemetry_codec]")
{
    std::array<std::byte, 64> buffer{};
    const auto next = std::copy(wireFrame.begin(), wireFrame.end(), buffer.begin());
    std::copy(wireFrame.begin(), wireFrame.end(), next);

    for (std::size_t size = wireFrame.size() + 1; size <= buffer.size(); ++size) {
        CAPTURE(size);
        const auto result = decode(std::span<const std::byte>{buffer}.first(size));

        REQUIRE(result.error == DecodeError::InvalidSize);
        REQUIRE_FALSE(static_cast<bool>(result));
    }
}

TEST_CASE("decode validates every header byte", "[telemetry_codec]")
{
    const DecodeError errors[] = {
        DecodeError::InvalidMagic,
        DecodeError::InvalidMagic,
        DecodeError::InvalidMagic,
        DecodeError::InvalidMagic,
        DecodeError::UnsupportedVersion,
        DecodeError::UnsupportedVersion,
        DecodeError::InvalidDeclaredFrameSize,
        DecodeError::InvalidDeclaredFrameSize,
    };

    for (std::size_t offset = 0; offset < 8; ++offset) {
        CAPTURE(offset);
        auto frame = wireFrame;
        frame[offset] ^= std::byte{0x01};

        const auto result = decode(frame);
        REQUIRE(result.error == errors[offset]);
        REQUIRE_FALSE(static_cast<bool>(result));
    }
}

TEST_CASE("decode accepts a frame at an unaligned buffer offset", "[telemetry_codec]")
{
    alignas(std::uint64_t) std::array<std::byte, 33> buffer{};
    std::copy(wireFrame.begin(), wireFrame.end(), buffer.begin() + 1);

    const auto result = decode(std::span<const std::byte>{buffer}.subspan(1));

    REQUIRE(result.error == DecodeError::None);
    REQUIRE(result.record == sampleRecord);
}

TEST_CASE("decode errors have readable descriptions", "[telemetry_codec]")
{
    struct Case {
        DecodeError error;
        std::string_view description;
    };
    const Case cases[] = {
        {DecodeError::None, "none"},
        {DecodeError::InvalidSize, "invalid frame size"},
        {DecodeError::InvalidMagic, "invalid magic"},
        {DecodeError::UnsupportedVersion, "unsupported version"},
        {DecodeError::InvalidDeclaredFrameSize, "invalid declared frame size"},
        {static_cast<DecodeError>(255), "unknown decode error"},
    };

    for (const auto &test : cases) {
        CAPTURE(test.description);
        REQUIRE(std::string_view{toString(test.error)} == test.description);
    }
}
