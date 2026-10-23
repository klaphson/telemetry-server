#include "Buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

TEST_CASE("client buffer compaction preserves unread bytes", "[buffer]")
{
    constexpr std::size_t threshold = 4096;
    Buffer buffer;
    buffer.data.assign(threshold, std::byte{'x'});
    buffer.data.push_back(std::byte{0});
    buffer.data.push_back(std::byte{0xff});
    const auto original = buffer.data;

    buffer.compact();
    REQUIRE(buffer.data == original);

    buffer.offset = threshold - 1;
    buffer.compact();
    REQUIRE(buffer.data == original);
    REQUIRE(buffer.pendingBytes() == 3);

    buffer.offset = threshold;
    buffer.compact();
    REQUIRE(buffer.offset == 0);
    REQUIRE(buffer.data == std::vector<std::byte>{std::byte{0}, std::byte{0xff}});

    buffer.offset = buffer.data.size();
    buffer.compact();
    REQUIRE(buffer.data.empty());
    REQUIRE(buffer.offset == 0);
    REQUIRE(buffer.pendingBytes() == 0);
}
