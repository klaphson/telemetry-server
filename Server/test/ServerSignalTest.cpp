#include "Server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>

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
        if (fd >= 0) {
            close(fd);
        }
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};

struct SignalPipe {
    int fds[2] = {-1, -1};
    SignalPipe()
    {
        REQUIRE(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    }
    ~SignalPipe()
    {
        close(fds[0]);
        close(fds[1]);
    }
    SignalPipe(const SignalPipe &) = delete;
    SignalPipe &operator=(const SignalPipe &) = delete;
};

} // namespace

// Inject complete signalfd records through a nonblocking pipe to control event
// ordering without delivering process-wide signals or forking children.
// This fixture lives in its own executable, separate from the backpressure fixture.
struct ServerTestAccess {
    static constexpr pid_t readerPid = 1234;
    using SignalState = Server::SignalState;
    using State = Server::State;

    Descriptor epoll{epoll_create1(EPOLL_CLOEXEC)};
    Descriptor listener{socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    SignalPipe signals;
    State state = State::Running;
    Server server;

    ServerTestAccess()
    {
        REQUIRE(server.addEpollFd(epoll.fd, listener.fd, EPOLLIN));
        const int clientFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        REQUIRE(clientFd >= 0);
        server.m_clients.emplace(clientFd, Server::ClientState{});
        REQUIRE(server.addEpollFd(epoll.fd, clientFd, EPOLLIN));
    }

    void signal(int number, int code = 0, pid_t pid = readerPid, int status = 0)
    {
        signalfd_siginfo info{};
        info.ssi_signo = static_cast<std::uint32_t>(number);
        info.ssi_code = code;
        info.ssi_pid = static_cast<std::uint32_t>(pid);
        info.ssi_status = status;
        REQUIRE(write(signals.fds[1], &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info)));
    }

    bool readSignals(SignalState &result)
    {
        return server.handleSignalFd(signals.fds[0], readerPid, result);
    }

    bool event(std::uint32_t events = EPOLLIN)
    {
        epoll_event event{};
        event.data.fd = signals.fds[0];
        event.events = events;
        return server.handleSignalEvent(event, epoll.fd, listener.fd, state, readerPid);
    }

    bool hasClients() const
    {
        return !server.m_clients.empty();
    }
};

TEST_CASE_METHOD(ServerTestAccess, "empty signal input keeps the server running", "[signals]")
{
    SignalState result{};
    REQUIRE(readSignals(result));
    REQUIRE_FALSE(result.shutdownRequested);
    REQUIRE_FALSE(result.readerTerminated);
    REQUIRE(event()); // A stale readiness event is harmless.
    REQUIRE(state == State::Running);
    REQUIRE(listener.fd >= 0);
    REQUIRE(hasClients());
}

TEST_CASE_METHOD(ServerTestAccess, "shutdown signals close ingress and enter draining", "[signals]")
{
    const int number = GENERATE(SIGINT, SIGTERM);
    signal(number);
    REQUIRE(event());
    REQUIRE(state == State::Draining);
    REQUIRE(listener.fd == -1);
    REQUIRE_FALSE(hasClients());

    signal(number);
    REQUIRE(event());
    REQUIRE(state == State::Draining);
    REQUIRE(listener.fd == -1);
    REQUIRE_FALSE(hasClients());
}

TEST_CASE_METHOD(ServerTestAccess, "reader termination is fatal while running or draining",
                 "[signals]")
{
    const auto [code, status] = GENERATE(table<int, int>(
        {{CLD_EXITED, 0}, {CLD_EXITED, 1}, {CLD_KILLED, SIGKILL}, {CLD_DUMPED, SIGABRT}}));
    const bool draining = GENERATE(false, true);
    if (draining) {
        signal(SIGTERM);
        REQUIRE(event());
        REQUIRE(state == State::Draining);
    }
    signal(SIGCHLD, code, readerPid, status);
    REQUIRE_FALSE(event());
    REQUIRE(state == (draining ? State::Draining : State::Running));
}

TEST_CASE_METHOD(ServerTestAccess, "nonterminal reader events preserve ingress", "[signals]")
{
    const int code = GENERATE(static_cast<int>(CLD_STOPPED), CLD_CONTINUED, CLD_TRAPPED, 0);
    signal(SIGCHLD, code);
    REQUIRE(event());
    REQUIRE(state == State::Running);
    REQUIRE(listener.fd >= 0);
    REQUIRE(hasClients());
}

TEST_CASE_METHOD(ServerTestAccess, "unrelated signals and child exits are ignored", "[signals]")
{
    SECTION("another child terminates")
    {
        const int code = GENERATE(static_cast<int>(CLD_EXITED), CLD_KILLED, CLD_DUMPED);
        signal(SIGCHLD, code, readerPid + 1);
    }
    SECTION("unmanaged signal")
    {
        signal(SIGUSR1);
    }
    REQUIRE(event());
    REQUIRE(state == State::Running);
    REQUIRE(listener.fd >= 0);
    REQUIRE(hasClients());
}

TEST_CASE_METHOD(ServerTestAccess, "signal batches accumulate shutdown and reader termination",
                 "[signals]")
{
    const bool shutdownFirst = GENERATE(false, true);
    if (shutdownFirst) {
        signal(SIGTERM);
    }
    signal(SIGCHLD, CLD_EXITED);
    if (!shutdownFirst) {
        signal(SIGINT);
    }
    signal(SIGCHLD, CLD_CONTINUED);
    signal(SIGCHLD, CLD_EXITED, readerPid + 1);

    SignalState result{};
    REQUIRE(readSignals(result));
    REQUIRE(result.shutdownRequested);
    REQUIRE(result.readerTerminated);

    SignalState remaining{};
    REQUIRE(readSignals(remaining));
    REQUIRE_FALSE(remaining.shutdownRequested);
    REQUIRE_FALSE(remaining.readerTerminated);
}

TEST_CASE_METHOD(ServerTestAccess, "reader failure takes precedence over graceful shutdown",
                 "[signals]")
{
    const bool shutdownFirst = GENERATE(false, true);
    if (shutdownFirst) {
        signal(SIGTERM);
    }
    signal(SIGCHLD, CLD_KILLED, readerPid, SIGKILL);
    if (!shutdownFirst) {
        signal(SIGTERM);
    }
    REQUIRE_FALSE(event());
    REQUIRE(state == State::Running);
}
