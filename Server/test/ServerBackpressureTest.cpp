#include "IpcQueue.hpp"
#include "Server.hpp"
#include "TelemetryCodec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/epoll.h>
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

    explicit DescriptorPair(bool pipe = false)
    {
        if (pipe) {
            REQUIRE(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
        } else {
            REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == 0);
        }
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
    DescriptorPair pipe{true};
    DescriptorPair client;
    int clientFd = client.fds[0];
    IpcQueue queue;
    bool paused = false;
    Server::State state = Server::State::Running;
    Server server;

    ServerTestAccess()
    {
        REQUIRE(server.addEpollFd(epoll.fd, listener.fds[0], EPOLLIN | EPOLLERR | EPOLLHUP));
        REQUIRE(server.addEpollFd(epoll.fd, pipe.fds[1], EPOLLERR | EPOLLHUP));
        REQUIRE(server.addEpollFd(epoll.fd, clientFd, EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP));
        server.m_clients.emplace(clientFd, Server::ClientState{});
        client.fds[0] = -1; // Server now owns this descriptor.
    }

    bool event(int fd, std::uint32_t events)
    {
        epoll_event event{};
        event.data.fd = fd;
        event.events = events;
        return server.handleEvent(event, epoll.fd, listener.fds[0], pipe.fds[1], queue, paused,
                                  state);
    }

    bool interest(bool enabled)
    {
        return server.updateIngressInterest(epoll.fd, listener.fds[0], enabled);
    }

    bool enqueue(std::span<const std::byte> frame)
    {
        return server.enqueueFrame(epoll.fd, pipe.fds[1], queue, frame);
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

    void fillPipe()
    {
        const std::array<char, 4096> bytes{};
        while (true) {
            const ssize_t count = write(pipe.fds[1], bytes.data(), bytes.size());
            if (count < 0) {
                REQUIRE(errno == EAGAIN);
                return;
            }
            REQUIRE(count > 0);
        }
    }

    std::string readPipe()
    {
        std::string result;
        std::array<char, 4096> bytes{};
        while (true) {
            const ssize_t count = read(pipe.fds[0], bytes.data(), bytes.size());
            if (count < 0) {
                REQUIRE(errno == EAGAIN);
                return result;
            }
            REQUIRE(count > 0);
            result.append(bytes.data(), static_cast<std::size_t>(count));
        }
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

TEST_CASE_METHOD(ServerTestAccess, "ingress interest toggles readability and retains hangups",
                 "[backpressure]")
{
    send(listener.fds[1], "pending connection");
    send(client.fds[1], frameBytes());
    REQUIRE((readyEvents(listener.fds[0]) & EPOLLIN) != 0U);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);

    REQUIRE(interest(false));
    REQUIRE((readyEvents(listener.fds[0]) & EPOLLIN) == 0U);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) == 0U);

    REQUIRE(interest(true));
    REQUIRE((readyEvents(listener.fds[0]) & EPOLLIN) != 0U);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);

    REQUIRE(interest(false));
    REQUIRE(shutdown(client.fds[1], SHUT_WR) == 0);
    REQUIRE((readyEvents(clientFd) & EPOLLRDHUP) != 0U);
}

TEST_CASE_METHOD(ServerTestAccess, "a full pipe pauses ingress at the high watermark",
                 "[backpressure]")
{
    fillPipe();
    queue.data.assign(IpcQueue::pauseIngressThreshold - frameSize, std::byte{'x'});
    const auto first = frameBytes(3);
    const auto second = frameBytes(4);
    const auto third = frameBytes(5);
    const auto partial = third.substr(0, 10);
    // Process both complete frames before pausing and retain the partial third frame.
    send(client.fds[1], first + second + partial);
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(paused);
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold + frameSize);
    const std::string_view queued(reinterpret_cast<const char*>(queue.data.data()),
                                  queue.data.size());
    REQUIRE(queued.ends_with(first + second));
    REQUIRE(pendingInput() == partial);
    REQUIRE(hasClient());

    send(client.fds[1], third.substr(partial.size()));
    REQUIRE((readyEvents(clientFd) & EPOLLIN) == 0U);
    REQUIRE(event(clientFd, EPOLLIN)); // Readiness already returned before pausing.
    REQUIRE(pendingInput() == partial);
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold + frameSize);

    readPipe();
    std::string forwarded;
    while (!queue.empty()) {
        REQUIRE(event(pipe.fds[1], EPOLLOUT));
        forwarded += readPipe();
    }
    REQUIRE_FALSE(paused);
    REQUIRE(forwarded ==
            std::string(IpcQueue::pauseIngressThreshold - frameSize, 'x') + first + second);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(readPipe() == third);
    REQUIRE(pendingInput().empty());
}

TEST_CASE_METHOD(ServerTestAccess, "client reading stops before the next receive when paused",
                 "[backpressure]")
{
    fillPipe();
    queue.data.assign(IpcQueue::pauseIngressThreshold - frameSize, std::byte{'x'});
    // The first frame reaches the watermark. Finish the current 4096-byte receive,
    // but leave the second receive's worth of frames in the socket.
    std::string frames;
    for (std::uint64_t timestamp = 0; frames.size() < 8192; ++timestamp) {
        frames += frameBytes(timestamp);
    }
    send(client.fds[1], frames);
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(paused);
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold - frameSize + 4096);
    REQUIRE(hasClient());
    REQUIRE(pendingInput().empty());
    const std::string_view queued(reinterpret_cast<const char*>(queue.data.data()),
                                  queue.data.size());
    REQUIRE(queued.ends_with(frames.substr(0, 4096)));
    std::array<char, 8192> remaining{};
    REQUIRE(recv(clientFd, remaining.data(), remaining.size(), MSG_PEEK) == 4096);
    REQUIRE(std::string_view(remaining.data(), 4096) == frames.substr(4096));
}

TEST_CASE_METHOD(ServerTestAccess, "pipe events resume ingress only at the low watermark",
                 "[backpressure]")
{
    fillPipe(); // Keep pending bytes deterministic even for a stale EPOLLOUT event.
    REQUIRE(interest(false));
    paused = true;
    queue.data.assign(IpcQueue::resumeIngressThreshold + 1, std::byte{'x'});
    send(client.fds[1], frameBytes());

    REQUIRE(event(pipe.fds[1], EPOLLOUT));
    REQUIRE(paused);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) == 0U);

    queue.offset = 1;
    REQUIRE(event(pipe.fds[1], EPOLLOUT));
    REQUIRE_FALSE(paused);
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);
}

TEST_CASE_METHOD(ServerTestAccess, "draining never resumes ingress", "[backpressure]")
{
    REQUIRE(interest(false));
    paused = true;
    drainState();
    const auto record = frameBytes();
    REQUIRE(queue.appendFrame(std::as_bytes(std::span(record))));
    // Shutdown has already unregistered the listener; trying to resume would fail.
    REQUIRE(epoll_ctl(epoll.fd, EPOLL_CTL_DEL, listener.fds[0], nullptr) == 0);
    REQUIRE(event(pipe.fds[1], EPOLLOUT));
    REQUIRE(queue.empty());
    REQUIRE(paused);
    REQUIRE(readPipe() == record);
    REQUIRE((readyEvents(pipe.fds[1]) & EPOLLOUT) == 0U);
}

TEST_CASE_METHOD(ServerTestAccess, "paused listener ignores stale readability but reports errors",
                 "[backpressure]")
{
    paused = true;
    // This socketpair cannot accept(); attempting to accept the stale event fails.
    REQUIRE(event(listener.fds[0], EPOLLIN));
    REQUIRE_FALSE(event(listener.fds[0], EPOLLIN | EPOLLERR));
}

TEST_CASE_METHOD(ServerTestAccess, "paused clients still process disconnect events",
                 "[backpressure]")
{
    paused = true;
    REQUIRE(event(clientFd, EPOLLRDHUP));
    REQUIRE_FALSE(hasClient());
}

TEST_CASE_METHOD(ServerTestAccess, "hard queue limit is a fatal enqueue failure", "[backpressure]")
{
    queue.data.assign(IpcQueue::maxBufferSize, std::byte{'x'});
    const auto original = queue.data;
    const auto overflow = frameBytes();
    REQUIRE_FALSE(enqueue(std::as_bytes(std::span(overflow))));
    REQUIRE(queue.data == original);
    REQUIRE(readPipe().empty());
}

TEST_CASE_METHOD(ServerTestAccess, "ingress update failures propagate from event handling",
                 "[backpressure]")
{
    SECTION("pause failure")
    {
        fillPipe();
        queue.data.assign(IpcQueue::pauseIngressThreshold - frameSize, std::byte{'x'});
        send(client.fds[1], frameBytes());
        REQUIRE(epoll_ctl(epoll.fd, EPOLL_CTL_DEL, listener.fds[0], nullptr) == 0);
        REQUIRE_FALSE(event(clientFd, EPOLLIN));
        REQUIRE_FALSE(paused);
    }
    SECTION("resume failure")
    {
        paused = true;
        REQUIRE(epoll_ctl(epoll.fd, EPOLL_CTL_DEL, clientFd, nullptr) == 0);
        REQUIRE_FALSE(event(pipe.fds[1], EPOLLOUT));
        REQUIRE(paused);
    }
}
