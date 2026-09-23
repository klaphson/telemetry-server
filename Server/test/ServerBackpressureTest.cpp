#include "IpcQueue.hpp"
#include "Server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>

namespace
{

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

    bool enqueue(const std::string &record)
    {
        return server.enqueueRecord(epoll.fd, pipe.fds[1], queue, record);
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
        return server.m_clients.at(clientFd).inputBuffer;
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
    send(client.fds[1], "record\n");
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
    queue.buffer.assign(IpcQueue::pauseIngressThreshold - 2, 'x');
    // Both complete records in this receive must survive the pause; preserve the
    // partial record so that it can be completed after ingress resumes.
    send(client.fds[1], "a\nb\r\npartial");
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(paused);
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold + 2);
    REQUIRE(queue.buffer.ends_with("a\nb\n"));
    REQUIRE(pendingInput() == "partial");
    REQUIRE(hasClient());

    send(client.fds[1], " remainder\n");
    REQUIRE((readyEvents(clientFd) & EPOLLIN) == 0U);
    REQUIRE(event(clientFd, EPOLLIN)); // Readiness already returned before pausing.
    REQUIRE(pendingInput() == "partial");
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold + 2);

    readPipe();
    std::string forwarded;
    while (!queue.empty()) {
        REQUIRE(event(pipe.fds[1], EPOLLOUT));
        forwarded += readPipe();
    }
    REQUIRE_FALSE(paused);
    REQUIRE(forwarded == std::string(IpcQueue::pauseIngressThreshold - 2, 'x') + "a\nb\n");
    REQUIRE((readyEvents(clientFd) & EPOLLIN) != 0U);
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(readPipe() == "partial remainder\n");
    REQUIRE(pendingInput().empty());
}

TEST_CASE_METHOD(ServerTestAccess, "client reading stops before the next receive when paused",
                 "[backpressure]")
{
    fillPipe();
    queue.buffer.assign(IpcQueue::pauseIngressThreshold - 2, 'x');
    // More than one 4096-byte receive, with only one complete record in the first.
    send(client.fds[1], "a\n" + std::string(8192, 'b') + "\n");
    REQUIRE(event(clientFd, EPOLLIN));
    REQUIRE(paused);
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold);
    char next = 0;
    REQUIRE(recv(clientFd, &next, 1, MSG_PEEK) == 1);
    REQUIRE(next == 'b');
}

TEST_CASE_METHOD(ServerTestAccess, "pipe events resume ingress only at the low watermark",
                 "[backpressure]")
{
    fillPipe(); // Keep pending bytes deterministic even for a stale EPOLLOUT event.
    REQUIRE(interest(false));
    paused = true;
    queue.buffer.assign(IpcQueue::resumeIngressThreshold + 1, 'x');
    send(client.fds[1], "waiting\n");

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
    queue.buffer = "last record\n";
    // Shutdown has already unregistered the listener; trying to resume would fail.
    REQUIRE(epoll_ctl(epoll.fd, EPOLL_CTL_DEL, listener.fds[0], nullptr) == 0);
    REQUIRE(event(pipe.fds[1], EPOLLOUT));
    REQUIRE(queue.empty());
    REQUIRE(paused);
    REQUIRE(readPipe() == "last record\n");
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
    queue.buffer.assign(IpcQueue::maxBufferSize, 'x');
    const std::string original = queue.buffer;
    REQUIRE_FALSE(enqueue("overflow"));
    REQUIRE(queue.buffer == original);
    REQUIRE(readPipe().empty());
}

TEST_CASE_METHOD(ServerTestAccess, "ingress update failures propagate from event handling",
                 "[backpressure]")
{
    SECTION("pause failure")
    {
        fillPipe();
        queue.buffer.assign(IpcQueue::pauseIngressThreshold - 2, 'x');
        send(client.fds[1], "a\n");
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
