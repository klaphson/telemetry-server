#include "TelemetryCodec.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using telemetry::protocol::encode;
using telemetry::protocol::Frame;
using telemetry::protocol::kFrameSize;
using telemetry::protocol::TelemetryRecord;
using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(const std::string &message)
{
    throw std::runtime_error(message);
}

void require(bool condition, const std::string &message)
{
    if (!condition) {
        fail(message);
    }
}

std::string systemError(std::string_view operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

class Descriptor
{
  public:
    explicit Descriptor(int fd = -1) : m_fd(fd) {}
    ~Descriptor()
    {
        if (m_fd >= 0) {
            close(m_fd);
        }
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    Descriptor(Descriptor &&other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Descriptor &operator=(Descriptor &&other) noexcept
    {
        if (this != &other) {
            if (m_fd >= 0) {
                close(m_fd);
            }
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const
    {
        return m_fd;
    }

  private:
    int m_fd;
};

class TemporaryDirectory
{
  public:
    explicit TemporaryDirectory(std::string_view prefix)
    {
        std::string pattern =
            (fs::temp_directory_path() / (std::string(prefix) + "XXXXXX")).string();
        const char* const folder = mkdtemp(pattern.data());
        if (folder == nullptr) {
            fail(systemError("mkdtemp"));
        }
        m_path = folder;
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    [[nodiscard]] const fs::path &path() const
    {
        return m_path;
    }

  private:
    fs::path m_path;
};

Frame record(std::uint32_t sensor, std::uint64_t sequence)
{
    return encode(TelemetryRecord{.sensorId = sensor,
                                  .metricId = 7,
                                  .timestampNs = sequence,
                                  .value = static_cast<double>(sensor)});
}

std::vector<char> readFile(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail("cannot open file " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::uint16_t allocatePort()
{
    Descriptor sock(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(sock.get() >= 0, systemError("socket"));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            systemError("bind"));
    socklen_t size = sizeof(address);
    require(getsockname(sock.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0,
            systemError("getsockname"));
    return ntohs(address.sin_port);
}

Descriptor connectClient(std::uint16_t port)
{
    Descriptor sock(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    require(sock.get() >= 0, systemError("socket client"));
    timeval timeout{5, 0};
    require(setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
            systemError("setsockopt"));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            systemError("connect"));
    return sock;
}

void sendAll(int socketFd, std::span<const std::byte> bytes)
{
    while (!bytes.empty()) {
        const ssize_t count = send(socketFd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            fail(systemError("send"));
        }
        bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
}

std::string logContents(const fs::path &path)
{
    const auto content = readFile(path);
    return {content.begin(), content.end()};
}

class ServerProcess
{
  public:
    ServerProcess(const fs::path &binary, std::string_view mode, std::uint16_t port,
                  const fs::path &data, const fs::path &logPath)
        : m_logPath(logPath)
    {
        const std::string argPort = std::to_string(port);
        const std::string argMode(mode);
        const std::string argData = data.string();
        const std::string executable = binary.string();
        Descriptor log(openLog(logPath));
        const pid_t pid = fork();
        require(pid >= 0, systemError("fork"));
        if (pid == 0) {
            (void)setsid();
            if (dup2(log.get(), STDOUT_FILENO) < 0 || dup2(log.get(), STDERR_FILENO) < 0) {
                _exit(127);
            }
            execl(executable.c_str(), executable.c_str(), "--reader-mode", argMode.c_str(),
                  "--port", argPort.c_str(), "--data-file", argData.c_str(), "--quiet",
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        m_pid = pid;
    }
    ~ServerProcess()
    {
        if (m_pid > 0) {
            if (!m_cleanShutdown) {
                // Also kill Reader when the Server process has already exited.
                (void)kill(-m_pid, SIGKILL);
                if (!m_reaped) {
                    (void)kill(m_pid, SIGKILL);
                }
            }
            if (!m_reaped) {
                int status = 0;
                while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
    }
    ServerProcess(const ServerProcess &) = delete;
    ServerProcess &operator=(const ServerProcess &) = delete;
    [[nodiscard]] pid_t pid() const
    {
        return m_pid;
    }
    void sendSignal(int sig) const
    {
        require(kill(m_pid, sig) == 0, systemError("kill"));
    }
    void acceptCleanup()
    {
        m_cleanShutdown = true;
    }
    int waitExit(std::chrono::milliseconds timeout)
    {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (!running()) {
                return m_status;
            }
            std::this_thread::sleep_for(20ms);
        }
        fail("server exit exceeded timeout");
    }

    [[nodiscard]] bool running()
    {
        if (m_reaped) {
            return false;
        }
        int status = 0;
        const pid_t code = waitpid(m_pid, &status, WNOHANG);
        if (code == 0) {
            return true;
        }
        if (code == m_pid) {
            m_reaped = true;
            m_status = status;
            return false;
        }
        fail(systemError("waitpid"));
    }
    void waitReady()
    {
        const auto deadline = Clock::now() + 10s;
        while (Clock::now() < deadline) {
            const std::string logs = logContents(m_logPath);
            if (logs.find("[server] listening on") != std::string::npos &&
                logs.find("[reader] reading telemetry from shared memory") != std::string::npos) {
                return;
            }
            require(running(), "server exited before listening");
            std::this_thread::sleep_for(20ms);
        }
        fail("server startup timeout");
    }
    void stopGracefully()
    {
        require(running(), "server exited before graceful shutdown");
        require(kill(m_pid, SIGTERM) == 0, systemError("SIGTERM"));
        const auto deadline = Clock::now() + 12s;
        while (running() && Clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        require(!running(), "server did not exit after SIGTERM");
        require(WIFEXITED(m_status) && WEXITSTATUS(m_status) == 0,
                "server did not exit successfully");
        m_cleanShutdown = true;
    }

  private:
    static int openLog(const fs::path &path)
    {
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        require(fd >= 0, systemError("open log"));
        return fd;
    }
    pid_t m_pid = -1;
    bool m_reaped = false;
    bool m_cleanShutdown = false;
    int m_status = -1;
    fs::path m_logPath;
};

void waitForFile(const fs::path &path, std::uint64_t expectedBytes, ServerProcess &server)
{
    const auto deadline = Clock::now() + 12s;
    while (Clock::now() < deadline) {
        require(server.running(), "server exited before persistence completed");
        std::error_code error;
        if (fs::exists(path, error) && fs::file_size(path, error) >= expectedBytes && !error) {
            return;
        }
        std::this_thread::sleep_for(20ms);
    }
    fail("timed out waiting for all persisted bytes");
}

pid_t readerPid(pid_t parent)
{
    const fs::path children =
        fs::path("/proc") / std::to_string(parent) / "task" / std::to_string(parent) / "children";
    std::ifstream input(children);
    pid_t child = 0;
    input >> child;
    require(child > 0, "cannot identify process-mode Reader PID from /proc");
    return child;
}

// An orphan that is already a zombie is no longer executing. The test
// deliberately does not wait for PID 1 to reap it.
bool readerStopped(pid_t pid)
{
    std::ifstream input(fs::path("/proc") / std::to_string(pid) / "stat");
    if (!input) {
        return true;
    }
    std::string stat;
    std::getline(input, stat);
    const auto endName = stat.rfind(')');
    require(endName != std::string::npos && endName + 2 < stat.size(), "malformed /proc/PID/stat");
    const char state = stat[endName + 2];
    return state == 'Z' || state == 'X' || state == 'x';
}

void runFailureCase(const fs::path &binary, std::string_view scenario)
{
    TemporaryDirectory temp("telemetry-lifecycle-");
    const fs::path data = temp.path() / "data.bin";
    const fs::path log = temp.path() / "server.log";
    const auto port = allocatePort();
    const std::string_view mode = scenario == "graceful-thread" ? "thread" : "process";
    ServerProcess process(binary, mode, port, data, log);

    try {
        process.waitReady();

        if (scenario == "reader-killed") {
            const pid_t child = readerPid(process.pid());
            require(kill(child, SIGKILL) == 0, "cannot terminate Reader");
            const int status = process.waitExit(6s);
            require(WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS,
                    "Server did not fail when its Reader was killed");
            require(logContents(log).find("[main] reader terminated by signal=") !=
                        std::string::npos,
                    "Reader termination was not reported");
        } else if (scenario == "parent-killed") {
            const pid_t child = readerPid(process.pid());
            process.sendSignal(SIGKILL);
            const int status = process.waitExit(6s);
            require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
                    "parent did not die by SIGKILL");
            const auto deadline = Clock::now() + 4s;
            while (!readerStopped(child) && Clock::now() < deadline) {
                std::this_thread::sleep_for(20ms);
            }
            if (!readerStopped(child)) {
                (void)kill(child, SIGKILL); // Do not leak a Reader if implementation regresses.
                fail("orphan Reader is still running after parent death");
            }
        } else if (scenario == "stopped-reader-timeout") {
            const pid_t child = readerPid(process.pid());
            require(kill(child, SIGSTOP) == 0, "cannot suspend Reader");
            std::this_thread::sleep_for(100ms);
            process.sendSignal(SIGTERM);
            const int status = process.waitExit(15s);
            require(WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS,
                    "Server returned success despite a Reader timeout");
            require(logContents(log).find("reader shutdown timeout") != std::string::npos,
                    "Reader timeout was not logged");
        } else if (scenario == "graceful-process" || scenario == "graceful-thread") {
            constexpr std::size_t frames = 256;
            {
                auto socket = connectClient(port);
                for (std::size_t index = 1; index <= frames; ++index) {
                    const Frame frame = record(55, index);
                    sendAll(socket.get(), frame);
                }
            }
            waitForFile(data, frames * kFrameSize, process);
            process.stopGracefully();
            require(logContents(log).find("[reader] EOF") != std::string::npos,
                    "Reader did not finish gracefully");
            const auto stored = readFile(data);
            require(stored.size() == frames * kFrameSize, "incorrect persisted size");
            for (std::size_t i = 0; i < frames; ++i) {
                const Frame frame = record(55, i + 1);
                require(std::memcmp(stored.data() + i * kFrameSize, frame.data(), kFrameSize) == 0,
                        "persisted frame mismatch after graceful shutdown");
            }
        } else {
            fail("unknown failure scenario");
        }
        process.acceptCleanup();
        std::cout << "scenario=" << scenario << " PASS\n";
    } catch (...) {
        std::cerr << "Server log (" << scenario << "):\n" << logContents(log) << '\n';
        throw;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc != 4 || std::string_view(argv[2]) != "--scenario") {
            std::cerr << "Usage: telemetry-process-failure-test SERVER --scenario "
                         "reader-killed|parent-killed|stopped-reader-timeout|"
                         "graceful-process|graceful-thread\n";
            return 2;
        }
        const fs::path binary = fs::absolute(argv[1]);
        require(fs::is_regular_file(binary), "Server binary not found");
        runFailureCase(binary, argv[3]);
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "process lifetime test FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
