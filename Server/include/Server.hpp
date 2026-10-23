#ifndef SERVER_HPP
#define SERVER_HPP

#include "Buffer.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <sys/types.h>

struct epoll_event;

struct ServerConfig {
    std::uint16_t port = 9000;
};

class SharedRingBuffer;

class Server
{
  public:
    explicit Server(ServerConfig config = {});
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    Server(Server &&) = delete;
    Server &operator=(Server &&) = delete;

    // Borrows the ring and descriptors; the caller owns their lifetimes.
    int run(SharedRingBuffer &ring, int dataReadyFd, int spaceReadyFd, int signalFd,
            pid_t readerPid);

  private:
    friend struct ServerTestAccess;

    enum class State { Running, Draining };

    struct ClientState {
        Buffer inputBuffer;
    };

    struct SignalState {
        bool shutdownRequested = false;
        bool readerTerminated = false;
    };

    ServerConfig m_config;
    std::unordered_map<int, ClientState> m_clients;

    // Socket setup
    int createSocket() const;
    bool setSocketOptions(int fd) const;
    bool bindSocket(int fd) const;

    // Event handling
    bool addEpollFd(int epollFd, int fd, std::uint32_t events) const;
    bool modifyEpollFd(int epoll_fd, int fd, std::uint32_t events) const;

    // Returns false only when the event requires stopping the server.
    bool handleEvent(const epoll_event &event, int epollFd, int listenFd, SharedRingBuffer &ring,
                     int dataReadyFd, int spaceReadyFd, bool &ingressPaused, State &state);

    // Client connections
    bool acceptClients(int epoll_fd, int listen_fd);
    bool readClient(int epollFd, int clientFd, SharedRingBuffer &ring, int dataReadyFd,
                    bool &pauseIngressRequested);
    void removeClient(int epoll_fd, int client_fd);
    void removeAllClients(int epollFd);

    // Shared ring output
    bool enqueueFrame(SharedRingBuffer &ring, int dataReadyFd,
                      std::span<const std::byte> frame) const;

    bool handleSignalEvent(const epoll_event &event, int epollFd, int &listenFd, State &state,
                           pid_t readerPid);
    bool handleSignalFd(int signalFd, pid_t readerPid, SignalState &signalState) const;
    void beginDraining(int epollFd, int &listenFd);

    bool updateIngressInterest(int epollFd, int listenFd, bool enabled) const;
};

#endif // SERVER_HPP
