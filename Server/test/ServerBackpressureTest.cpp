#include "Server.hpp"
#include "SharedRingBuffer.hpp"
#include "TelemetryCodec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>
#include <string_view>

namespace
{

// Strings retain embedded zero bytes and make concatenation/fragmentation convenient.
std::string frameBytes(std::uint64_t timestamp = 3)
{
    const auto frame = telemetry::protocol::encode({1, 2, timestamp, 1.0});
    return {reinterpret_cast<const char*>(frame.data()), frame.size()};
}

constexpr auto frameSize = telemetry::protocol::kFrameSize;
constexpr std::size_t pauseIngressBytes = 3 * 1024 * 1024;
constexpr std::size_t resumeIngressBytes = 1 * 1024 * 1024;

struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value)
    {
        REQUIRE(fd >= 0);
    }
    ~Descriptor()
    {
        close(fd);
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};

struct DescriptorPair {
    int fds[2] = {-1, -1};

    DescriptorPair()
    {
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
    }
    ~DescriptorPair()
    {
        for (int fd : fds) {
            if (fd >= 0) {
                close(fd);
            }
        }
    }
    DescriptorPair(const DescriptorPair &) = delete;
    DescriptorPair &operator=(const DescriptorPair &) = delete;
};

} // namespace

// A friend fixture exercises individual event handlers without running the server
// loop or binding its fixed TCP port. All descriptors are real and nonblocking.
struct ServerTestAccess {
    Descriptor epoll{epoll_create1(EPOLL_CLOEXEC)};
    DescriptorPair listener;
    DescriptorPair client;
    int clientFd = client.fds[0];
    bool paused = false;
    Server::State state = Server::State::Running;
    Server server;
    SharedRingBuffer ring;

    Descriptor dataReady{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    Descriptor spaceReady{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};

    ServerTestAccess()
    {
        REQUIRE(server.addEpollFd(epoll.fd, listener.fds[0], EPOLLIN | EPOLLERR | EPOLLHUP));
        REQUIRE(server.addEpollFd(epoll.fd, clientFd, EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP));
        server.m_clients.emplace(clientFd, Server::ClientState{});
        client.fds[0] = -1; // Server now owns this descriptor.
        REQUIRE(ring.create());
        REQUIRE(server.addEpollFd(epoll.fd, spaceReady.fd, EPOLLIN | EPOLLERR | EPOLLHUP));
    }

    bool event(int fd, std::uint32_t events)
    {
        epoll_event event{};
        event.data.fd = fd;
        event.events = events;
        return server.handleEvent(event, epoll.fd, listener.fds[0], ring, dataReady.fd,
                                  spaceReady.fd, paused, state);
    }

    bool interest(bool enabled)
    {
        return server.updateIngressInterest(epoll.fd, listener.fds[0], enabled);
    }

    bool enqueue(std::span<const std::byte> frame)
    {
        return server.enqueueFrame(ring, frame);
    }

    void drainState()
    {
        state = Server::State::Draining;
    }
    bool hasClient() const
    {
        return server.m_clients.contains(clientFd);
    }
    std::string pendingInput() const
    {
        const auto &input = server.m_clients.at(clientFd).inputBuffer;
        if (input.empty()) {
            return {};
        }
        return {reinterpret_cast<const char*>(input.data.data() + input.offset),
                input.pendingBytes()};
    }

    void send(int fd, const std::string &data)
    {
        REQUIRE(write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    }

    std::string fillRing(std::size_t bytes)
    {
        REQUIRE(ring.empty());
        REQUIRE(bytes <= SharedRingBuffer::capacityBytes);
        REQUIRE(bytes % frameSize == 0);
        const auto frame = frameBytes(0);
        std::string expected;
        expected.reserve(bytes);
        while (expected.size() < bytes) {
            REQUIRE(ring.tryPush(std::as_bytes(std::span(frame))));
            expected += frame;
        }
        return expected;
    }

    std::string readRing()
    {
        std::string result;
        telemetry::protocol::Frame frame{};
        while (ring.tryPop(frame)) {
            result.append(reinterpret_cast<const char*>(frame.data()), frame.size());
        }
        if (!result.empty()) {
            REQUIRE(eventfd_write(spaceReady.fd, 1) == 0);
        }
        return result;
    }

    std::uint32_t readyEvents(int fd)
    {
        std::array<epoll_event, 8> events{};
        const int count = epoll_wait(epoll.fd, events.data(), static_cast<int>(events.size()), 0);
        REQUIRE(count >= 0);
        for (int i = 0; i < count; ++i) {
            const auto &event = events[static_cast<std::size_t>(i)];
            if (event.data.fd == fd) {
                return event.events;
            }
        }
        return 0;
    }
};

TEST_CASE_METHOD(ServerTestAccess, "paused clients leave epoll but keep their socket data",
                 "[backpressure]")
{
    send(listener.fds[1], "pending connection");
    send(client.fds[1], frameBytes());
    REQUIRE((readyEvents(listener.fds[0]) & EPOLLIN) != 0U);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);

    REQUIRE(interest(false));
    REQUIRE((readyEvents(listener.fds[0]) & EPOLLIN) == 0U);
    REQUIRE(readyEvents(clientFd) == 0U);
    REQUIRE(hasClient());

    // A peer may finish sending while ingress is paused.
    REQUIRE(shutdown(client.fds[1], SHUT_WR) == 0);
    REQUIRE(readyEvents(clientFd) == 0U);
    REQUIRE(hasClient());

    REQUIRE(interest(true));
    REQUIRE((readyEvents(listener.fds[0]) & EPOLLIN) != 0U);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);
    REQUIRE((readyEvents(clientFd) & EPOLLRDHUP) != 0U);
}

TEST_CASE_METHOD(ServerTestAccess, "ring occupancy pauses ingress at the high watermark",
                 "[backpressure]")
{
    const auto initial = fillRing(pauseIngressBytes - frameSize);
    const auto first = frameBytes(3);
    const auto second = frameBytes(4);
    const auto third = frameBytes(5);
    const auto partial = third.substr(0, 10);
    // Process both complete frames before pausing and retain the partial third frame.
    send(client.fds[1], first + second + partial);
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(paused);
    REQUIRE(ring.pendingBytes() == pauseIngressBytes + frameSize);
    REQUIRE(pendingInput() == partial);
    REQUIRE(hasClient());

    send(client.fds[1], third.substr(partial.size()));
    REQUIRE((readyEvents(clientFd) & EPOLLIN) == 0U);
    REQUIRE(event(clientFd, EPOLLIN)); // Readiness already returned before pausing.
    REQUIRE(pendingInput() == partial);
    REQUIRE(ring.pendingBytes() == pauseIngressBytes + frameSize);

    const auto forwarded = readRing();
    REQUIRE((readyEvents(spaceReady.fd) & EPOLLIN) != 0U);
    REQUIRE(event(spaceReady.fd, EPOLLIN));
    REQUIRE((readyEvents(spaceReady.fd) & EPOLLIN) == 0U);
    REQUIRE_FALSE(paused);
    REQUIRE(forwarded == initial + first + second);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(readRing() == third);
    REQUIRE(pendingInput().empty());
}

TEST_CASE_METHOD(ServerTestAccess, "client reading stops before the next receive when paused",
                 "[backpressure]")
{
    const auto initial = fillRing(pauseIngressBytes - frameSize);
    // The first frame reaches the watermark. Finish the current 4096-byte receive,
    // but leave the second receive's worth of frames in the socket.
    std::string frames;
    for (std::uint64_t timestamp = 0; frames.size() < 8192; ++timestamp) {
        frames += frameBytes(timestamp);
    }
    send(client.fds[1], frames);
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(paused);
    REQUIRE(ring.pendingBytes() == pauseIngressBytes - frameSize + 4096);
    REQUIRE(hasClient());
    REQUIRE(pendingInput().empty());
    std::array<char, 8192> remaining{};
    REQUIRE(recv(clientFd, remaining.data(), remaining.size(), MSG_PEEK) == 4096);
    REQUIRE(std::string_view(remaining.data(), 4096) == frames.substr(4096));
    REQUIRE(readRing() == initial + frames.substr(0, 4096));
}

TEST_CASE_METHOD(ServerTestAccess, "space notifications resume ingress only at the low watermark",
                 "[backpressure]")
{
    REQUIRE(interest(false));
    paused = true;
    fillRing(resumeIngressBytes + frameSize);
    send(client.fds[1], frameBytes());

    REQUIRE(eventfd_write(spaceReady.fd, 1) == 0);
    REQUIRE(event(spaceReady.fd, EPOLLIN));
    REQUIRE(paused);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) == 0U);

    telemetry::protocol::Frame frame{};
    REQUIRE(ring.tryPop(frame));
    REQUIRE(ring.pendingBytes() == resumeIngressBytes);
    REQUIRE(eventfd_write(spaceReady.fd, 1) == 0);
    REQUIRE(event(spaceReady.fd, EPOLLIN));
    REQUIRE_FALSE(paused);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);
    REQUIRE((readyEvents(spaceReady.fd) & EPOLLIN) == 0U);
}

TEST_CASE_METHOD(ServerTestAccess, "draining never resumes ingress", "[backpressure]")
{
    REQUIRE(interest(false));
    paused = true;
    drainState();
    const auto record = frameBytes();
    REQUIRE(enqueue(std::as_bytes(std::span(record))));
    // Shutdown has already unregistered the listener; trying to resume would fail.
    REQUIRE(epoll_ctl(epoll.fd, EPOLL_CTL_DEL, listener.fds[0], nullptr) == 0);
    REQUIRE(readRing() == record);
    REQUIRE(event(spaceReady.fd, EPOLLIN));
    REQUIRE(ring.empty());
    REQUIRE(paused);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) == 0U);
    REQUIRE((readyEvents(spaceReady.fd) & EPOLLIN) == 0U);
}

TEST_CASE_METHOD(ServerTestAccess, "paused listener ignores stale readability but reports errors",
                 "[backpressure]")
{
    paused = true;
    // This socketpair cannot accept(); attempting to accept the stale event fails.
    REQUIRE(event(listener.fds[0], EPOLLIN));
    REQUIRE_FALSE(event(listener.fds[0], EPOLLIN | EPOLLERR));
}

TEST_CASE_METHOD(ServerTestAccess, "stale hangup cannot discard paused client data",
                 "[backpressure]")
{
    REQUIRE(interest(false));
    paused = true;
    const auto expected = frameBytes(17);
    send(client.fds[1], expected);
    REQUIRE(shutdown(client.fds[1], SHUT_WR) == 0);

    // Simulate a stale event returned just before EPOLL_CTL_DEL.
    REQUIRE(event(clientFd, EPOLLRDHUP | EPOLLHUP));
    REQUIRE(hasClient());
    REQUIRE(ring.empty());

    REQUIRE(interest(true));
    paused = false;
    REQUIRE(event(clientFd, EPOLLIN | EPOLLRDHUP));
    REQUIRE_FALSE(hasClient());
    REQUIRE(readRing() == expected);
}

TEST_CASE_METHOD(ServerTestAccess, "backpressure preserves remaining frames after sender EOF",
                 "[backpressure]")
{
    const auto initial = fillRing(pauseIngressBytes - frameSize);
    std::string frames;
    for (std::uint64_t timestamp = 0; frames.size() < 8192; ++timestamp) {
        frames += frameBytes(timestamp);
    }
    send(client.fds[1], frames);
    REQUIRE(shutdown(client.fds[1], SHUT_WR) == 0);

    // The first recv fills the ring over the high watermark.
    REQUIRE(event(clientFd, EPOLLIN | EPOLLRDHUP));
    REQUIRE(paused);
    REQUIRE(hasClient());
    REQUIRE(readyEvents(clientFd) == 0U);

    // Even an already-queued hangup must not discard the socket's 4096 bytes.
    REQUIRE(event(clientFd, EPOLLRDHUP | EPOLLHUP));
    REQUIRE(hasClient());
    const auto forwarded = readRing();
    REQUIRE(event(spaceReady.fd, EPOLLIN));
    REQUIRE_FALSE(paused);

    // Re-registering EPOLLIN allows the remaining bytes and EOF to be read.
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);
    REQUIRE(event(clientFd, EPOLLIN | EPOLLRDHUP));
    REQUIRE_FALSE(hasClient());
    REQUIRE(forwarded + readRing() == initial + frames);
}

TEST_CASE_METHOD(ServerTestAccess, "a full ring is a fatal enqueue failure", "[backpressure]")
{
    const auto original = fillRing(SharedRingBuffer::capacityBytes);
    const auto overflow = frameBytes();
    REQUIRE_FALSE(enqueue(std::as_bytes(std::span(overflow))));
    REQUIRE(ring.pendingBytes() == SharedRingBuffer::capacityBytes);
    eventfd_t notifications{};
    REQUIRE(eventfd_read(dataReady.fd, &notifications) == -1);
    REQUIRE(errno == EAGAIN);
    REQUIRE(readRing() == original);
}

TEST_CASE_METHOD(ServerTestAccess, "one recv batch generates one reader notification",
                 "[backpressure]")
{
    const auto first = frameBytes(3);
    const auto second = frameBytes(4);
    send(client.fds[1], first + second);
    REQUIRE(event(clientFd, EPOLLIN));

    eventfd_t notifications{};
    REQUIRE(eventfd_read(dataReady.fd, &notifications) == 0);
    REQUIRE(notifications == 1);
    REQUIRE(eventfd_read(dataReady.fd, &notifications) == -1);
    REQUIRE(errno == EAGAIN);
    REQUIRE(readRing() == first + second);
}

TEST_CASE_METHOD(ServerTestAccess, "two recv batches generate two notifications", "[backpressure]")
{
    std::string frames;
    for (std::uint64_t timestamp = 0; timestamp < 256; ++timestamp) {
        frames += frameBytes(timestamp);
    }
    REQUIRE(frames.size() == 2 * 4096);
    send(client.fds[1], frames);
    REQUIRE(event(clientFd, EPOLLIN));

    eventfd_t notifications{};
    REQUIRE(eventfd_read(dataReady.fd, &notifications) == 0);
    REQUIRE(notifications == 2);
    REQUIRE(readRing() == frames);
}

TEST_CASE_METHOD(ServerTestAccess, "partial TCP frames do not notify early", "[backpressure]")
{
    const auto expected = frameBytes(7);
    send(client.fds[1], expected.substr(0, 11));
    REQUIRE(event(clientFd, EPOLLIN));

    eventfd_t notifications{};
    REQUIRE(eventfd_read(dataReady.fd, &notifications) == -1);
    REQUIRE(errno == EAGAIN);
    REQUIRE(ring.empty());

    send(client.fds[1], expected.substr(11));
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(eventfd_read(dataReady.fd, &notifications) == 0);
    REQUIRE(notifications == 1);
    REQUIRE(readRing() == expected);
}

TEST_CASE_METHOD(ServerTestAccess, "valid frames notify before a later protocol error",
                 "[backpressure]")
{
    const auto valid = frameBytes(8);
    send(client.fds[1], valid + std::string(frameSize, '\0'));
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE_FALSE(hasClient());

    eventfd_t notifications{};
    REQUIRE(eventfd_read(dataReady.fd, &notifications) == 0);
    REQUIRE(notifications == 1);
    REQUIRE(readRing() == valid);
}

TEST_CASE_METHOD(ServerTestAccess, "eventfd failures propagate from IPC handling", "[backpressure]")
{
    SECTION("reader notification failure")
    {
        REQUIRE(close(dataReady.fd) == 0);
        dataReady.fd = -1;
        send(client.fds[1], frameBytes());
        REQUIRE_FALSE(event(clientFd, EPOLLIN));
    }
    SECTION("space notification read failure")
    {
        REQUIRE(close(spaceReady.fd) == 0);
        spaceReady.fd = -1;
        REQUIRE_FALSE(event(spaceReady.fd, EPOLLIN));
    }
    SECTION("space descriptor error")
    {
        REQUIRE_FALSE(event(spaceReady.fd, EPOLLERR));
    }
    SECTION("space descriptor hangup")
    {
        REQUIRE_FALSE(event(spaceReady.fd, EPOLLHUP));
    }
}

TEST_CASE_METHOD(ServerTestAccess, "ingress update failures propagate from event handling",
                 "[backpressure]")
{
    SECTION("pause failure")
    {
        fillRing(pauseIngressBytes - frameSize);
        send(client.fds[1], frameBytes());
        REQUIRE(epoll_ctl(epoll.fd, EPOLL_CTL_DEL, listener.fds[0], nullptr) == 0);
        REQUIRE_FALSE(event(clientFd, EPOLLIN));
        REQUIRE_FALSE(paused);
    }
    SECTION("resume failure")
    {
        // The normal resume uses EPOLL_CTL_ADD because pausing unregisters
        // the client. Force that ADD to fail with EEXIST by leaving the
        // client registered, rather than removing it (which makes ADD work).
        paused = true;
        REQUIRE(eventfd_write(spaceReady.fd, 1) == 0);
        REQUIRE_FALSE(event(spaceReady.fd, EPOLLIN));
        REQUIRE(paused);
        REQUIRE(hasClient());
    }
}
