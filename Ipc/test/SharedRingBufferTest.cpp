#include "SharedRingBuffer.hpp"
#include "TelemetryCodec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sched.h>
#include <span>
#include <vector>
#include <cerrno>
#include <cstddef>
#include <cstdlib>

TEST_CASE("push and pop preserve an exact frame", "[shared_ring]")
{
    using namespace telemetry::protocol;

    SharedRingBuffer ring;

    REQUIRE(ring.create());
    REQUIRE(ring.empty());
    REQUIRE(ring.pendingFrames() == 0);

    const Frame expected = encode(
        TelemetryRecord{.sensorId = 42, .metricId = 7, .timestampNs = 123456, .value = 23.5});

    REQUIRE(ring.tryPush(expected));

    REQUIRE(!ring.empty());
    REQUIRE(ring.pendingFrames() == 1);
    REQUIRE(ring.pendingBytes() == kFrameSize);

    Frame actual{};

    REQUIRE(ring.tryPop(actual));

    REQUIRE(actual == expected);
    REQUIRE(ring.empty());
}

TEST_CASE("frame written after fork is visible to child", "[shared_ring][fork]")
{
    using namespace telemetry::protocol;

    SharedRingBuffer ring;

    REQUIRE(ring.create());

    const Frame expected =
        encode(TelemetryRecord{.sensorId = 100, .metricId = 200, .timestampNs = 300, .value = 4.5});

    int readyPipe[2]{};

    REQUIRE(pipe2(readyPipe, O_CLOEXEC) == 0);

    const pid_t childPid = fork();

    REQUIRE(childPid >= 0);

    if (childPid == 0) {
        close(readyPipe[1]);

        char token{};

        ssize_t bytesRead = -1;

        do {
            bytesRead = read(readyPipe[0], &token, sizeof(token));
        } while (bytesRead == -1 && errno == EINTR);

        Frame actual{};

        const bool success = bytesRead == 1 && ring.tryPop(actual) && actual == expected;

        close(readyPipe[0]);

        _exit(success ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    close(readyPipe[0]);

    REQUIRE(ring.tryPush(expected));

    const char token = 'x';

    REQUIRE(write(readyPipe[1], &token, sizeof(token)) == 1);

    close(readyPipe[1]);

    int status = 0;

    REQUIRE(waitpid(childPid, &status, 0) == childPid);

    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == EXIT_SUCCESS);

    REQUIRE(ring.empty());
}

TEST_CASE("consumer may drain frames after producer closes", "[shared_ring]")
{
    using namespace telemetry::protocol;

    SharedRingBuffer ring;

    REQUIRE(ring.create());

    const Frame expected =
        encode(TelemetryRecord{.sensorId = 1, .metricId = 2, .timestampNs = 3, .value = 1.0});

    REQUIRE(ring.tryPush(expected));

    ring.closeProducer();

    REQUIRE(ring.producerClosed());

    Frame actual{};

    REQUIRE(ring.tryPop(actual));
    REQUIRE(actual == expected);
    REQUIRE(ring.empty());

    REQUIRE(!ring.tryPush(expected));
}

TEST_CASE("batch transfers preserve ordering and reject malformed spans", "[shared_ring][batch]")
{
    using namespace telemetry::protocol;

    SharedRingBuffer ring;
    REQUIRE(ring.create());

    const std::array<Frame, 3> expected{{
        encode(TelemetryRecord{.timestampNs = 10}),
        encode(TelemetryRecord{.timestampNs = 11}),
        encode(TelemetryRecord{.timestampNs = 12}),
    }};
    std::array<std::byte, 3 * kFrameSize> bytes{};
    std::copy(reinterpret_cast<const std::byte*>(expected.data()),
              reinterpret_cast<const std::byte*>(expected.data()) + bytes.size(),
              bytes.begin());

    REQUIRE(ring.tryPushBatch({bytes.data(), bytes.size() - 1}) == 0);
    REQUIRE(ring.tryPushBatch({}) == 0);
    REQUIRE(ring.pendingFrames() == 0);
    REQUIRE(ring.tryPushBatch(bytes) == 3);
    REQUIRE(ring.pendingFrames() == 3);

    std::array<std::byte, 2 * kFrameSize> first{};
    REQUIRE(ring.tryPopBatch({first.data(), first.size() - 1}) == 0);
    REQUIRE(ring.tryPopBatch(first) == 2);
    REQUIRE(std::equal(first.begin(), first.end(), bytes.begin()));
    REQUIRE(ring.pendingFrames() == 1);

    Frame final{};
    REQUIRE(ring.tryPop(final)); // scalar API still inter-operates
    REQUIRE(final == expected[2]);
    REQUIRE(ring.tryPopBatch(first) == 0);
    REQUIRE(ring.empty());

    ring.closeProducer();
    REQUIRE(ring.tryPushBatch(bytes) == 0);
}

TEST_CASE("batch wraparound and partial ring capacity are safe", "[shared_ring][batch]")
{
    using namespace telemetry::protocol;

    SharedRingBuffer ring;
    REQUIRE(ring.create());

    // Move head and tail to the last two slots without reallocating the ring.
    std::vector<std::byte> fill((SharedRingBuffer::capacityFrames - 2) * kFrameSize,
                                std::byte{0x19});
    REQUIRE(ring.tryPushBatch(fill) == SharedRingBuffer::capacityFrames - 2);
    REQUIRE(ring.tryPopBatch(fill) == SharedRingBuffer::capacityFrames - 2);
    REQUIRE(ring.empty());

    const std::array<Frame, 4> frames{{
        encode(TelemetryRecord{.timestampNs = 1}),
        encode(TelemetryRecord{.timestampNs = 2}),
        encode(TelemetryRecord{.timestampNs = 3}),
        encode(TelemetryRecord{.timestampNs = 4}),
    }};
    const auto input = std::as_bytes(std::span{frames});
    REQUIRE(ring.tryPushBatch(input) == 4);

    std::array<Frame, 4> result{};
    REQUIRE(ring.tryPopBatch(std::as_writable_bytes(std::span{result})) == 4);
    REQUIRE(result == frames);

    std::vector<std::byte> full(SharedRingBuffer::capacityBytes, std::byte{0x31});
    REQUIRE(ring.tryPushBatch(full) == SharedRingBuffer::capacityFrames);
    REQUIRE(ring.full());
    REQUIRE(ring.tryPushBatch(input) == 0);

    Frame discarded{};
    REQUIRE(ring.tryPop(discarded));
    REQUIRE(ring.tryPushBatch(input) == 1); // only one free slot
    REQUIRE(ring.full());
    REQUIRE(ring.tryPopBatch(full) == SharedRingBuffer::capacityFrames);
    REQUIRE(ring.empty());
}

TEST_CASE("forked producer and consumer transfer batched frames concurrently",
          "[shared_ring][batch][fork]")
{
    using namespace telemetry::protocol;

    SharedRingBuffer ring;
    REQUIRE(ring.create());

    constexpr std::uint64_t totalFrames = 25000;
    constexpr std::size_t batchFrames = 64;
    const pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        alarm(15); // An isolated child must not hang the test indefinitely.
        std::array<Frame, batchFrames> frames{};
        std::uint64_t expected = 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};

        while (expected <= totalFrames && std::chrono::steady_clock::now() < deadline) {
            const std::size_t count = ring.tryPopBatch(std::as_writable_bytes(std::span{frames}));
            if (count == 0) {
                sched_yield();
                continue;
            }
            for (std::size_t i = 0; i < count; ++i) {
                const auto decoded = decode(frames[i]);
                if (!decoded || decoded.record.timestampNs != expected++) {
                    _exit(EXIT_FAILURE);
                }
            }
        }
        _exit(expected == totalFrames + 1 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    std::uint64_t sent = 1;
    std::array<Frame, batchFrames> frames{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (sent <= totalFrames && std::chrono::steady_clock::now() < deadline) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(batchFrames, totalFrames - sent + 1));
        for (std::size_t i = 0; i < count; ++i) {
            frames[i] = encode(TelemetryRecord{.timestampNs = sent + i});
        }
        const auto n = ring.tryPushBatch(std::as_bytes(std::span{frames}.first(count)));
        sent += n;
        if (n == 0) {
            sched_yield();
        }
    }
    if (sent != totalFrames + 1) {
        kill(pid, SIGKILL);
    }

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(sent == totalFrames + 1);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == EXIT_SUCCESS);
    REQUIRE(ring.empty());
}
