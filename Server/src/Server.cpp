#include "Server.hpp"
#include "IpcQueue.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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

constexpr std::uint16_t kPort = 9000;
constexpr int kMaxEvents = 64;
constexpr std::size_t kReadBufferSize = 4096;
constexpr std::size_t kMaxClientBuffer = 64 * 1024;

} // namespace

Server::~Server()
{
    for (const auto &[fd, client] : m_clients) {
        close(fd);
    }
}

int Server::run(int pipeWriteFd, int signalFd)
{
    if (!setNonBlocking(pipeWriteFd)) {
        perror("fcntl pipe O_NONBLOCK");
        return EXIT_FAILURE;
    }

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

    // Add pipe without EPOLLOUT initially. We enable it only when data is queued.
    if (!addEpollFd(epoll_fd, pipeWriteFd, EPOLLERR | EPOLLHUP)) {
        perror("epoll_ctl ADD pipe");
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

    IpcQueue ipcQueue;
    std::vector<epoll_event> events(kMaxEvents);

    std::cout << "[server] pid=" << getpid() << '\n'
              << "[server] listening on 0.0.0.0:" << kPort << '\n';

    State state = State::Running;
    int result = EXIT_FAILURE;
    bool running = true;

    while (running) {
        if (state == State::Draining && ipcQueue.empty()) {

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
                if (!handleSignalEvent(event, epoll_fd, listen_fd, state)) {
                    result = EXIT_FAILURE;
                    running = false;
                    break;
                }

                continue;
            }

            if (!handleEvent(event, epoll_fd, listen_fd, pipeWriteFd, ipcQueue)) {
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

bool Server::setNonBlocking(int fd) const
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
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
    address.sin_port = htons(kPort);
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

bool Server::handleEvent(const epoll_event &event, int epollFd, int listenFd, int pipeWriteFd,
                         IpcQueue &ipcQueue)
{
    const int fd = event.data.fd;
    const std::uint32_t eventMask = event.events;

    if (fd == listenFd) {
        if ((eventMask & EPOLLIN) != 0U) {
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

    if (fd == pipeWriteFd) {
        if ((eventMask & EPOLLOUT) != 0U) {
            if (!flushIpcQueue(pipeWriteFd, ipcQueue)) {
                return false;
            }

            if (!updatePipeInterest(epollFd, pipeWriteFd, !ipcQueue.empty())) {
                perror("epoll_ctl MOD pipe");
                return false;
            }
        }

        if ((eventMask & (EPOLLERR | EPOLLHUP)) != 0U) {
            std::cerr << "[server] reader pipe closed/error\n";
            return false;
        }

        return true;
    }

    if ((eventMask & EPOLLIN) != 0U) {
        if (!readClient(epollFd, fd, pipeWriteFd, ipcQueue)) {
            return false;
        }
    }

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

bool Server::readClient(int epoll_fd, int client_fd, int pipe_fd, IpcQueue &ipcQueue)
{
    auto it = m_clients.find(client_fd);
    if (it == m_clients.end()) {
        return true;
    }

    ClientState &client = it->second;
    char buffer[kReadBufferSize];

    while (true) {
        const ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);

        if (n > 0) {
            client.inputBuffer.append(buffer, static_cast<std::size_t>(n));

            if (client.inputBuffer.size() > kMaxClientBuffer) {
                std::cerr << "[server] client fd=" << client_fd << " exceeded input buffer limit\n";
                removeClient(epoll_fd, client_fd);
                return true;
            }

            while (true) {
                const std::size_t newline = client.inputBuffer.find('\n');
                if (newline == std::string::npos) {
                    break;
                }

                std::string record = client.inputBuffer.substr(0, newline);
                client.inputBuffer.erase(0, newline + 1);

                if (!record.empty() && record.back() == '\r') {
                    record.pop_back();
                }

                if (record.empty()) {
                    continue;
                }

                std::cout << "[server] fd=" << client_fd << " record=" << record << '\n';

                if (!enqueueRecord(epoll_fd, pipe_fd, ipcQueue, record)) {
                    return false;
                }
            }

            continue;
        }

        if (n == 0) {
            removeClient(epoll_fd, client_fd);
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        perror("recv");
        removeClient(epoll_fd, client_fd);
        return true;
    }
}

void Server::removeClient(int epoll_fd, int client_fd)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
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

bool Server::enqueueRecord(int epoll_fd, int pipe_fd, IpcQueue &queue,
                           const std::string &record) const
{
    if (!queue.appendLine(record)) {
        std::cerr << "[server] IPC queue limit reached (" << IpcQueue::maxBufferSize
                  << " bytes), dropping record\n";
        return true;
    }

    if (!flushIpcQueue(pipe_fd, queue)) {
        return false;
    }

    if (!updatePipeInterest(epoll_fd, pipe_fd, !queue.empty())) {
        perror("epoll_ctl MOD pipe");
        return false;
    }

    return true;
}

bool Server::flushIpcQueue(int pipe_fd, IpcQueue &queue) const
{
    while (!queue.empty()) {
        const char* data = queue.buffer.data() + queue.offset;
        const std::size_t remaining = queue.buffer.size() - queue.offset;

        const ssize_t n = write(pipe_fd, data, remaining);

        if (n > 0) {
            queue.offset += static_cast<std::size_t>(n);
            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }

        if (n == -1 && errno == EPIPE) {
            std::cerr << "[server] reader closed the pipe\n";
            return false;
        }

        perror("write(pipe)");
        return false;
    }

    queue.compactIfNeeded();
    return true;
}

bool Server::updatePipeInterest(int epoll_fd, int pipe_fd, bool want_epollout) const
{
    const std::uint32_t events = want_epollout
                                     ? static_cast<std::uint32_t>(EPOLLOUT | EPOLLERR | EPOLLHUP)
                                     : static_cast<std::uint32_t>(EPOLLERR | EPOLLHUP);

    return modifyEpollFd(epoll_fd, pipe_fd, events);
}

bool Server::handleSignalEvent(const epoll_event &event, int epollFd, int &listenFd, State &state)
{
    if ((event.events & EPOLLIN) != 0U) {
        bool shutdownRequested = false;

        if (!handleSignalFd(event.data.fd, shutdownRequested)) {
            return false;
        }

        if (shutdownRequested && state == State::Running) {
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

bool Server::handleSignalFd(int signalFd, bool &shutdownRequested) const
{
    while (true) {
        signalfd_siginfo info{};

        const ssize_t bytesRead = read(signalFd, &info, sizeof(info));

        if (bytesRead == static_cast<ssize_t>(sizeof(info))) {

            if (info.ssi_signo == static_cast<std::uint32_t>(SIGINT) ||
                info.ssi_signo == static_cast<std::uint32_t>(SIGTERM)) {

                std::cout << "[server] shutdown requested signal=" << info.ssi_signo << '\n';

                shutdownRequested = true;
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
        epoll_ctl(epollFd, EPOLL_CTL_DEL, listenFd, nullptr);

        close(listenFd);
        listenFd = -1;

        std::cout << "[server] stopped accepting clients\n";
    }

    removeAllClients(epollFd);
}
