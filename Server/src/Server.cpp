#include "Server.hpp"
#include "EventFd.hpp"
#include "SharedRingBuffer.hpp"
#include "TelemetryCodec.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

constexpr int kMaxEvents = 64;
constexpr std::size_t kReadBufferSize = 4096;
constexpr std::size_t kMaxClientBuffer = 64 * 1024;
constexpr std::size_t kPauseIngressBytes = 3 * 1024 * 1024;
constexpr std::size_t kResumeIngressBytes = 1 * 1024 * 1024;

} // namespace

Server::Server(ServerConfig config) : m_config(config), m_clients() {}

Server::~Server()
{
    for (const auto &[fd, client] : m_clients) {
        close(fd);
    }
}

int Server::run(SharedRingBuffer &ring, int dataReadyFd, int spaceReadyFd, int signalFd,
                pid_t readerPid)
{
    int listen_fd = createSocket();

    if (listen_fd == -1) {
        perror("createSocket");
        return EXIT_FAILURE;
    }

    if (!setSocketOptions(listen_fd)) {
        perror("setsockopt");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (!bindSocket(listen_fd)) {
        perror("bindSocket");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (listen(listen_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (!addEpollFd(epoll_fd, listen_fd, EPOLLIN | EPOLLERR | EPOLLHUP)) {
        perror("epoll_ctl ADD listen");
        close(epoll_fd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (!addEpollFd(epoll_fd, spaceReadyFd, EPOLLIN | EPOLLERR | EPOLLHUP)) {

        perror("epoll_ctl ADD spaceReady");
        close(epoll_fd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (!addEpollFd(epoll_fd, signalFd, EPOLLIN | EPOLLERR | EPOLLHUP)) {
        perror("epoll_ctl ADD signal");
        close(epoll_fd);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    std::vector<epoll_event> events(kMaxEvents);

    std::cout << "[server] pid=" << getpid() << '\n'
              << "[server] listening on 0.0.0.0:" << m_config.port << '\n';

    State state = State::Running;
    int result = EXIT_FAILURE;
    bool running = true;
    bool ingressPaused = false;

    while (running) {
        if (state == State::Draining && ring.empty()) {

            result = EXIT_SUCCESS;
            break;
        }

        const int ready = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);

        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const auto &event = events[static_cast<std::size_t>(i)];

            if (event.data.fd == signalFd) {
                if (!handleSignalEvent(event, epoll_fd, listen_fd, state, readerPid)) {
                    result = EXIT_FAILURE;
                    running = false;
                    break;
                }

                continue;
            }

            if (!handleEvent(event, epoll_fd, listen_fd, ring, dataReadyFd, spaceReadyFd,
                             ingressPaused, state)) {
                running = false;
                break;
            }
        }
    }

    removeAllClients(epoll_fd);

    if (listen_fd != -1) {
        close(listen_fd);
    }
    close(epoll_fd);

    return result;
}

int Server::createSocket() const
{
    return socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

bool Server::setSocketOptions(int fd) const
{
    int reuse = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != -1;
}

bool Server::bindSocket(int fd) const
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(m_config.port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    return bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != -1;
}

bool Server::addEpollFd(int epollFd, int fd, std::uint32_t events) const
{
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    return epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &event) != -1;
}

bool Server::modifyEpollFd(int epoll_fd, int fd, std::uint32_t events) const
{
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;

    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) != -1;
}

bool Server::removeEpollFd(int epollFd, int fd) const
{
    return epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr) != -1;
}

bool Server::handleEvent(const epoll_event &event, int epollFd, int listenFd,
                         SharedRingBuffer &ring, int dataReadyFd, int spaceReadyFd,
                         bool &ingressPaused, State &state)
{
    const int fd = event.data.fd;
    const std::uint32_t eventMask = event.events;

    if (fd == listenFd) {
        if (!ingressPaused && ((eventMask & EPOLLIN) != 0U)) {
            if (!acceptClients(epollFd, listenFd)) {
                return false;
            }
        }

        if ((eventMask & (EPOLLERR | EPOLLHUP)) != 0U) {
            std::cerr << "[server] listening socket error\n";
            return false;
        }

        return true;
    }

    if (fd == spaceReadyFd) {
        if ((eventMask & EPOLLIN) != 0U) {
            if (!telemetry::ipc::consumeEvent(spaceReadyFd)) {
                return false;
            }

            if (state == State::Running && ingressPaused &&
                ring.pendingBytes() <= kResumeIngressBytes) {

                if (!updateIngressInterest(epollFd, listenFd, true)) {
                    perror("epoll_ctl ingress resume");
                    return false;
                }

                ingressPaused = false;

                std::cout << "[server] ingress resumed, IPC pending=" << ring.pendingBytes()
                          << '\n';
            }
        }

        if ((eventMask & (EPOLLERR | EPOLLHUP)) != 0U) {
            std::cerr << "[server] space eventfd error\n";
            return false;
        }

        return true;
    }

    if (!ingressPaused && (eventMask & EPOLLIN) != 0U) {

        bool pauseRequested = false;

        if (!readClient(epollFd, fd, ring, dataReadyFd, pauseRequested)) {
            return false;
        }

        if (pauseRequested) {
            if (!updateIngressInterest(epollFd, listenFd, false)) {
                perror("epoll_ctl MOD ingress pause");
                return false;
            }

            ingressPaused = true;

            std::cout << "[server] ingress paused, ring buffer pending=" << ring.pendingBytes()
                      << '\n';

            return true;
        }
    }

    // During backpressure the client is intentionally absent from epoll.
    // A previously returned event may still contain RDHUP/HUP: do not
    // close the socket, since its receive buffer may contain unread frames.
    if (ingressPaused) {
        return true;
    }

    // Without a pause, readClient() drains the socket until EAGAIN or EOF.
    // An isolated hangup/error (without readable data) can be closed here.
    if ((eventMask & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0U) {
        if (m_clients.find(fd) != m_clients.end()) {
            removeClient(epollFd, fd);
        }
    }

    return true;
}

bool Server::acceptClients(int epoll_fd, int listen_fd)
{
    while (true) {
        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);

        const int client_fd = accept4(listen_fd, reinterpret_cast<sockaddr*>(&client_address),
                                      &client_length, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (client_fd >= 0) {
            if (!addEpollFd(epoll_fd, client_fd, EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
                perror("epoll_ctl ADD client");
                close(client_fd);
                continue;
            }

            m_clients.emplace(client_fd, ClientState{});

            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &client_address.sin_addr, ip, sizeof(ip));

            std::cout << "[server] client connected fd=" << client_fd << " from " << ip << ':'
                      << ntohs(client_address.sin_port) << '\n';
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        perror("accept4");
        return false;
    }
}

bool Server::readClient(int epollFd, int clientFd, SharedRingBuffer &ring, int dataReadyFd,
                        bool &pauseIngressRequested)
{
    using namespace telemetry::protocol;

    auto it = m_clients.find(clientFd);

    if (it == m_clients.end()) {
        return true;
    }

    ClientState &client = it->second;

    std::array<std::byte, kReadBufferSize> buffer{};

    while (true) {
        const ssize_t bytesRead = recv(clientFd, buffer.data(), buffer.size(), 0);

        if (bytesRead > 0) {
            const auto count = static_cast<std::size_t>(bytesRead);

            // Publish once per recv() batch rather than once per frame.
            bool publishedAny = false;
            const auto notifyPublished = [&]() {
                if (!publishedAny) {
                    return true;
                }
                publishedAny = false;
                return telemetry::ipc::notifyEvent(dataReadyFd);
            };

            client.inputBuffer.data.insert(client.inputBuffer.data.end(), buffer.begin(),
                                           buffer.begin() + static_cast<std::ptrdiff_t>(count));

            if (client.inputBuffer.pendingBytes() > kMaxClientBuffer) {

                std::cerr << "[server] client fd=" << clientFd << " exceeded input buffer limit\n";

                removeClient(epollFd, clientFd);

                return true;
            }

            while (client.inputBuffer.pendingBytes() >= kFrameSize) {

                const std::span<const std::byte> frame{
                    client.inputBuffer.data.data() + client.inputBuffer.offset, kFrameSize};

                const DecodeResult decoded = decode(frame);

                if (!decoded) {
                    std::cerr << "[server] client fd=" << clientFd
                              << " protocol error: " << toString(decoded.error) << '\n';

                    // Even if this frame is invalid, earlier valid frames
                    // from this recv() still need to wake the reader.
                    if (!notifyPublished()) {
                        return false;
                    }

                    removeClient(epollFd, clientFd);

                    return true;
                }

                const TelemetryRecord &record = decoded.record;

                if (!m_config.quiet) {
                    std::cout << "[server] fd=" << clientFd << " sensor=" << record.sensorId
                              << " metric=" << record.metricId
                              << " timestamp_ns=" << record.timestampNs << " value=" << record.value
                              << '\n';
                }

                if (!enqueueFrame(ring, frame)) {
                    // The parent will notify the reader when shutting down.
                    return false;
                }

                publishedAny = true;
                client.inputBuffer.offset += kFrameSize;

                if (ring.pendingBytes() >= kPauseIngressBytes) {
                    pauseIngressRequested = true;
                }
            }

            // Wake the reader after publishing all frames from this recv().
            if (!notifyPublished()) {
                return false;
            }

            client.inputBuffer.compact();

            if (pauseIngressRequested) {
                return true;
            }

            continue;
        }

        if (bytesRead == 0) {
            if (client.inputBuffer.pendingBytes() != 0) {
                std::cerr << "[server] client fd=" << clientFd
                          << " disconnected with partial frame: "
                          << client.inputBuffer.pendingBytes() << " bytes\n";
            }

            removeClient(epollFd, clientFd);

            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        perror("recv");

        removeClient(epollFd, clientFd);

        return true;
    }
}

void Server::removeClient(int epoll_fd, int client_fd)
{
    removeEpollFd(epoll_fd, client_fd);
    close(client_fd);
    m_clients.erase(client_fd);

    std::cout << "[server] client disconnected fd=" << client_fd << '\n';
}

void Server::removeAllClients(int epollFd)
{
    while (!m_clients.empty()) {
        const int clientFd = m_clients.begin()->first;

        removeClient(epollFd, clientFd);
    }
}

bool Server::enqueueFrame(SharedRingBuffer &ring, std::span<const std::byte> frame) const
{
    if (!ring.tryPush(frame)) {
        std::cerr << "[server] shared ring full or unavailable\n";
        return false;
    }

    return true;
}

bool Server::handleSignalEvent(const epoll_event &event, int epollFd, int &listenFd, State &state,
                               pid_t readerPid)
{
    if ((event.events & EPOLLIN) != 0U) {
        SignalState signalState{};

        if (!handleSignalFd(event.data.fd, readerPid, signalState)) {
            return false;
        }

        if (signalState.readerTerminated) {
            std::cerr << "[server] storage process unavailable\n";

            return false;
        }

        if (signalState.shutdownRequested && state == State::Running) {
            std::cout << "[server] entering draining state\n";

            state = State::Draining;
            beginDraining(epollFd, listenFd);
        }
    }

    if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
        std::cerr << "[server] signalfd error\n";
        return false;
    }

    return true;
}

bool Server::handleSignalFd(int signalFd, pid_t readerPid, SignalState &signalState) const
{
    while (true) {
        signalfd_siginfo info{};

        const ssize_t bytesRead = read(signalFd, &info, sizeof(info));

        if (bytesRead == static_cast<ssize_t>(sizeof(info))) {

            if (info.ssi_signo == static_cast<std::uint32_t>(SIGINT) ||
                info.ssi_signo == static_cast<std::uint32_t>(SIGTERM)) {

                std::cout << "[server] shutdown requested signal=" << info.ssi_signo << '\n';

                signalState.shutdownRequested = true;

                continue;
            }

            if (info.ssi_signo == static_cast<std::uint32_t>(SIGCHLD)) {

                // Child-state notifications can merge; only the delivered state is checked.
                if (info.ssi_pid != static_cast<std::uint32_t>(readerPid)) {

                    continue;
                }

                switch (info.ssi_code) {
                case CLD_EXITED:
                    std::cerr << "[server] reader exited code=" << info.ssi_status << '\n';

                    signalState.readerTerminated = true;

                    break;

                case CLD_KILLED:
                    std::cerr << "[server] reader killed by signal=" << info.ssi_status << '\n';

                    signalState.readerTerminated = true;

                    break;

                case CLD_DUMPED:
                    std::cerr << "[server] reader dumped core signal=" << info.ssi_status << '\n';

                    signalState.readerTerminated = true;

                    break;

                case CLD_STOPPED:
                    std::cout << "[server] reader stopped signal=" << info.ssi_status << '\n';

                    break;

                case CLD_CONTINUED:
                    std::cout << "[server] reader continued\n";

                    break;

                default:
                    break;
                }

                continue;
            }

            continue;
        }

        if (bytesRead == -1 && errno == EINTR) {
            continue;
        }

        if (bytesRead == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }

        if (bytesRead == 0) {
            std::cerr << "[server] unexpected signalfd EOF\n";
            return false;
        }

        perror("read(signalfd)");
        return false;
    }
}

void Server::beginDraining(int epollFd, int &listenFd)
{
    if (listenFd != -1) {
        removeEpollFd(epollFd, listenFd);

        close(listenFd);
        listenFd = -1;

        std::cout << "[server] stopped accepting clients\n";
    }

    removeAllClients(epollFd);
}

bool Server::updateIngressInterest(int epollFd, int listenFd, bool enabled) const
{
    std::uint32_t listenEvents = static_cast<std::uint32_t>(EPOLLERR | EPOLLHUP);

    if (enabled) {
        listenEvents |= static_cast<std::uint32_t>(EPOLLIN);
    }

    if (!modifyEpollFd(epollFd, listenFd, listenEvents)) {
        return false;
    }

    for (const auto &[clientFd, client] : m_clients) {
        static_cast<void>(client);

        if (enabled) {
            // An open peer may have sent FIN while paused. EPOLL_CTL_ADD
            // immediately reports unread data (and EOF when reached).
            if (!addEpollFd(epollFd, clientFd, EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
                return false;
            }
        } else {
            // EPOLLHUP/EPOLLERR are reported even without EPOLLIN.
            // Leaving the fd registered can spin epoll_wait() on a
            // half-closed socket. DEL pauses reads without closing it.
            if (!removeEpollFd(epollFd, clientFd)) {
                return false;
            }
        }
    }

    return true;
}
