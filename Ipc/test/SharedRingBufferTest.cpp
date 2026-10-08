#include "SharedRingBuffer.hpp"
#include "TelemetryCodec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
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