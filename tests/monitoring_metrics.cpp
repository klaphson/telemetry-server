// Native C++20 end-to-end checks for opt-in Server ingress monitoring.
#include "TelemetryCodec.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void require(bool condition, std::string_view error)
{
    if (!condition) {
        throw std::runtime_error(std::string(error));
    }
}

class TempDir
{
  public:
    TempDir()
    {
        std::string path = (fs::temp_directory_path() / "telemetry-metrics-XXXXXX").string();
        require(mkdtemp(path.data()) != nullptr, "mkdtemp failed");
        m_path = path;
    }
    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    [[nodiscard]] fs::path file(std::string_view name) const
    {
        return m_path / std::string(name);
    }

  private:
    fs::path m_path;
};

class Fd
{
  public:
    explicit Fd(int fd) : m_fd(fd)
    {
        require(fd >= 0, "file descriptor creation failed");
    }
    ~Fd()
    {
        if (m_fd >= 0) {
            close(m_fd);
        }
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    [[nodiscard]] int get() const
    {
        return m_fd;
    }

  private:
    int m_fd;
};

std::string readText(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "log or telemetry file not readable");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::uint16_t freePort()
{
    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "could not select ephemeral port");
    socklen_t length = sizeof(address);
    require(getsockname(fd.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "getsockname failed");
    return ntohs(address.sin_port);
}

class Child
{
  public:
    Child(const fs::path &binary, const fs::path &log, const fs::path &data, std::uint16_t port,
          std::string_view mode)
    {
        const std::string portArg = std::to_string(port);
        const std::string modeArg(mode);
        const std::string program = binary.string();
        const std::string dataArg = data.string();
        Fd logFd(open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        const pid_t pid = fork();
        require(pid >= 0, "fork failed");
        if (pid == 0) {
            (void)setsid();
            if (dup2(logFd.get(), STDOUT_FILENO) == -1 || dup2(logFd.get(), STDERR_FILENO) == -1) {
                _exit(127);
            }
            execl(program.c_str(), program.c_str(), "--port", portArg.c_str(), "--reader-mode",
                  modeArg.c_str(), "--stats-interval-ms", "100", "--data-file", dataArg.c_str(),
                  "--quiet", static_cast<char*>(nullptr));
            _exit(127);
        }
        m_pid = pid;
    }
    ~Child()
    {
        if (m_pid <= 0) {
            return;
        }
        if (!m_reaped) {
            (void)kill(-m_pid, SIGKILL);
            (void)kill(m_pid, SIGKILL);
            int status = 0;
            while (waitpid(m_pid, &status, 0) == -1 && errno == EINTR) {
            }
        }
    }
    [[nodiscard]] bool running()
    {
        if (m_reaped) {
            return false;
        }
        int status = 0;
        const pid_t got = waitpid(m_pid, &status, WNOHANG);
        if (got == m_pid) {
            m_reaped = true;
            m_status = status;
            return false;
        }
        require(got == 0, "waitpid failed");
        return true;
    }
    void stop()
    {
        require(kill(m_pid, SIGTERM) == 0, "SIGTERM failed");
        const auto deadline = Clock::now() + 12s;
        while (running() && Clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        require(!running(), "Server failed to stop");
        require(WIFEXITED(m_status) && WEXITSTATUS(m_status) == 0,
                "Server shutdown status is nonzero");
    }

  private:
    pid_t m_pid = -1;
    bool m_reaped = false;
    int m_status = -1;
};

void sendFrames(std::uint16_t port)
{
    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "client connect failed");
    for (std::uint64_t sequence = 1; sequence <= 64; ++sequence) {
        const auto wire = telemetry::protocol::encode({44, 7, sequence, 44.0});
        std::size_t sent = 0;
        while (sent < wire.size()) {
            const ssize_t amount =
                send(fd.get(), wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
            require(amount > 0, "client send failed");
            sent += static_cast<std::size_t>(amount);
        }
    }
}

bool hasMetricsLine(std::string_view logs, std::string_view prefix,
                    std::initializer_list<std::string_view> fields)
{
    while (!logs.empty()) {
        const auto end = logs.find('\n');
        if (end == std::string_view::npos) {
            return false; // Ignore a line that is still being written.
        }
        const auto line = logs.substr(0, end);
        const auto start = line.find(prefix);
        if (start != std::string_view::npos) {
            const auto metrics = line.substr(start);
            bool matches = true;
            for (const auto field : fields) {
                matches = matches && metrics.find(field) != std::string_view::npos;
            }
            if (matches) {
                return true;
            }
        }
        logs.remove_prefix(end + 1);
    }
    return false;
}

void run(const fs::path &binary, std::string_view mode)
{
    TempDir temp;
    const fs::path log = temp.file("server.log");
    const fs::path data = temp.file("records.bin");
    const std::uint16_t port = freePort();
    Child server(binary, log, data, port, mode);
    try {
        const auto readyDeadline = Clock::now() + 8s;
        bool ready = false;
        while (Clock::now() < readyDeadline) {
            require(server.running(), "Server exited during startup");
            const auto text = readText(log);
            if (text.find("[server] listening on") != std::string::npos &&
                text.find("[reader] reading telemetry from shared memory") != std::string::npos) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(20ms);
        }
        require(ready, "Server startup timeout");
        sendFrames(port);
        const auto deadline = Clock::now() + 5s;
        bool observed = false;
        while (Clock::now() < deadline) {
            require(server.running(), "Server exited before metrics observation");
            const std::string logs = readText(log);
            if (hasMetricsLine(logs, "[metrics] ",
                               {"accepted_total=64 ", "accepted_per_sec=", "backpressure_pauses=0 ",
                                "protocol_errors=0"}) &&
                hasMetricsLine(logs, "[reader-metrics] ",
                               {"consumed_total=64 ", "written_bytes_total=2048 ",
                                "synced_bytes_total=0 ", "unsynced_bytes=2048 ",
                                "written_bytes_per_sec=", "sync_successes=0 ", "storage_errors=0 ",
                                "status=running"})) {
                observed = true;
                break;
            }
            std::this_thread::sleep_for(20ms);
        }
        require(observed, "expected opt-in metrics line was not emitted");
        server.stop();
        require(hasMetricsLine(readText(log), "[reader-metrics] ",
                               {"consumed_total=64 ", "written_bytes_total=2048 ",
                                "synced_bytes_total=2048 ", "unsynced_bytes=0 ",
                                "sync_successes=1 ", "storage_errors=0 ", "status=success"}),
                "final Reader metrics did not confirm successful sync");
        require(fs::file_size(data) == 64 * telemetry::protocol::kFrameSize,
                "incorrect persisted frame count");
        const auto bytes = readText(data);
        for (std::uint64_t index = 0; index < 64; ++index) {
            const auto expected = telemetry::protocol::encode({44, 7, index + 1, 44.0});
            require(std::memcmp(bytes.data() + index * expected.size(), expected.data(),
                                expected.size()) == 0,
                    "persisted frames were corrupted or reordered");
        }
        std::cout << "metrics mode=" << mode << " accepted=64 verified=PASS\n";
    } catch (...) {
        try {
            std::cerr << "Server logs (" << mode << "):\n" << readText(log) << '\n';
        } catch (...) {
        }
        throw;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        require(argc == 3, "usage: telemetry-monitoring-test SERVER process|thread");
        const fs::path binary = fs::absolute(argv[1]);
        const std::string_view mode = argv[2];
        require(mode == "process" || mode == "thread", "unknown reader mode");
        run(binary, mode);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "monitoring integration FAILED: " << error.what() << '\n';
        return 1;
    }
}
