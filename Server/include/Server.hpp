#ifndef SERVER_HPP
#define SERVER_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

struct epoll_event;
struct IpcQueue;

class Server
{
public:
    Server() = default;
    ~Server();

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    Server(Server &&) = delete;
    Server &operator=(Server &&) = delete;

    // Borrows the descriptor; the caller closes it after run() returns.
    int run(int pipeWriteFd, int signalFd);

private:
    enum class State
    {
        Running,
        Draining
    };

    struct ClientState
    {
        std::string inputBuffer;
    };

    std::unordered_map<int, ClientState> m_clients;

    // Socket setup
    bool setNonBlocking(int fd) const;
    int createSocket() const;
    bool setSocketOptions(int fd) const;
    bool bindSocket(int fd) const;

    // Event handling
    bool addEpollFd(int epollFd, int fd, std::uint32_t events) const;
    bool modifyEpollFd(int epoll_fd, int fd, std::uint32_t events) const;
    // Returns false only when the event requires stopping the server.
    bool handleEvent(
        const epoll_event &event,
        int epollFd,
        int listenFd,
        int pipeWriteFd,
        IpcQueue &ipcQueue);

    // Client connections
    bool acceptClients(
        int epoll_fd,
        int listen_fd);
    bool readClient(
        int epoll_fd,
        int client_fd,
        int pipe_fd,
        IpcQueue &ipcQueue);
    void removeClient(
        int epoll_fd,
        int client_fd);
    void removeAllClients(
        int epollFd);

    // Pipe output
    bool enqueueRecord(
        int epoll_fd,
        int pipe_fd,
        IpcQueue &queue,
        const std::string &record) const;
    bool flushIpcQueue(int pipe_fd, IpcQueue &queue) const;
    bool updatePipeInterest(int epoll_fd, int pipe_fd, bool want_epollout) const;

    bool handleSignalEvent(
        const epoll_event &event,
        int epollFd,
        int &listenFd,
        State &state);
    bool handleSignalFd(
        int signalFd,
        bool &shutdownRequested) const;
    void beginDraining(
        int epollFd,
        int &listenFd);
};

#endif // SERVER_HPP
