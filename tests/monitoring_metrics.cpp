// Native C++20 end-to-end checks for opt-in Server ingress monitoring.
#include "TelemetryCodec.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
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
#include <optional>
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
          std::string_view mode, bool limitFile = false)
    {
        const std::string portArg = std::to_string(port);
        const std::string modeArg(mode);
        const std::string program = binary.string();
        const std::string dataArg = data.string();
        Fd logFd(open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        // A file-size limit applies to ALL regular files, including logs.
        // For fault injection route diagnostics through a pipe to the test parent.
        int pipeFds[2]{-1, -1};
        if (limitFile) {
            require(pipe2(pipeFds, O_CLOEXEC) == 0, "log pipe failed");
        }
        const pid_t pid = fork();
        if (pid < 0 && limitFile) {
            close(pipeFds[0]);
            close(pipeFds[1]);
        }
        require(pid >= 0, "fork failed");
        if (pid == 0) {
            (void)setsid();
            const int output = limitFile ? pipeFds[1] : logFd.get();
            if (limitFile) {
                close(pipeFds[0]);
                const rlimit limit{16, 16};
                if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
                    setrlimit(RLIMIT_FSIZE, &limit) == -1) {
                    _exit(127);
                }
            }
            if (dup2(output, STDOUT_FILENO) == -1 || dup2(output, STDERR_FILENO) == -1) {
                _exit(127);
            }
            execl(program.c_str(), program.c_str(), "--port", portArg.c_str(), "--reader-mode",
                  modeArg.c_str(), "--stats-interval-ms", "100", "--data-file", dataArg.c_str(),
                  "--quiet", static_cast<char*>(nullptr));
            _exit(127);
        }
        m_pid = pid;
        if (limitFile) {
            close(pipeFds[1]);
            m_logPipe = pipeFds[0];
            const int flags = fcntl(m_logPipe, F_GETFL);
            if (flags == -1 || fcntl(m_logPipe, F_SETFL, flags | O_NONBLOCK) == -1) {
                close(m_logPipe);
                m_logPipe = -1;
                (void)kill(m_pid, SIGKILL);
                int status = 0;
                while (waitpid(m_pid, &status, 0) == -1 && errno == EINTR) {
                }
                throw std::runtime_error("nonblocking log pipe setup failed");
            }
        }
    }
    ~Child()
    {
        if (m_logPipe >= 0) {
            close(m_logPipe);
        }
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
    [[nodiscard]] pid_t pid() const { return m_pid; }

    // Drain nonblocking pipe output from the size-limited child in the parent.
    void drainLog()
    {
        if (m_logPipe < 0) {
            return;
        }
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t size = read(m_logPipe, buffer.data(), buffer.size());
            if (size > 0) {
                m_capturedLog.append(buffer.data(), static_cast<std::size_t>(size));
                continue;
            }
            if (size < 0 && errno == EINTR) {
                continue;
            }
            require(size == 0 || errno == EAGAIN || errno == EWOULDBLOCK, "log pipe read failed");
            return;
        }
    }

    [[nodiscard]] const std::string &capturedLog() const { return m_capturedLog; }

    void waitFailure()
    {
        const auto deadline = Clock::now() + 8s;
        while (running() && Clock::now() < deadline) {
            drainLog();
            std::this_thread::sleep_for(20ms);
        }
        require(!running(), "Server did not exit after storage failure");
        drainLog();
        require(WIFEXITED(m_status) && WEXITSTATUS(m_status) != 0 &&
                    WEXITSTATUS(m_status) != 127,
                "expected application storage failure");
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
    int m_logPipe = -1;
    std::string m_capturedLog;
};

void sendFrames(std::uint16_t port, std::uint64_t count = 64)
{
    Fd fd(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "client connect failed");
    for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
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

std::optional<std::uint64_t> metricNumber(std::string_view line, std::string_view name)
{
    const std::string key = " " + std::string(name) + "=";
    const auto start = line.find(key);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    line.remove_prefix(start + key.size());
    const auto value = line.substr(0, line.find(' '));
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

bool validFinalLatency(std::string_view line)
{
    const auto writes = metricNumber(line, "write_latency_samples");
    const auto syncs = metricNumber(line, "sync_latency_samples");
    if (!writes || *writes == 0 || !syncs || *syncs != 1) {
        return false;
    }
    for (const std::string_view prefix : {"write", "sync"}) {
        const auto p50 = metricNumber(line, std::string(prefix) + "_p50_us");
        const auto p95 = metricNumber(line, std::string(prefix) + "_p95_us");
        const auto p99 = metricNumber(line, std::string(prefix) + "_p99_us");
        if (!p50 || !p95 || !p99 || *p50 == 0 || *p50 > *p95 || *p95 > *p99) {
            return false;
        }
    }
    return true;
}

bool hasMetricsLine(std::string_view logs, std::string_view prefix,
                    std::initializer_list<std::string_view> fields, bool checkLatency = false)
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
            if (matches && (!checkLatency || validFinalLatency(metrics))) {
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
        // Both Reader modes must report heartbeat while no client sends data.
        const auto idleDeadline = Clock::now() + 5s;
        bool responsive = false;
        while (Clock::now() < idleDeadline) {
            require(server.running(), "Server exited while idle");
            if (hasMetricsLine(readText(log), "[metrics] ",
                               {"accepted_total=0 ", "reader_health=responsive"})) {
                responsive = true;
                break;
            }
            std::this_thread::sleep_for(20ms);
        }
        require(responsive, "idle Reader heartbeat not observed");
        sendFrames(port);
        const auto deadline = Clock::now() + 5s;
        bool observed = false;
        while (Clock::now() < deadline) {
            require(server.running(), "Server exited before metrics observation");
            const std::string logs = readText(log);
            if (hasMetricsLine(logs, "[metrics] ",
                               {"accepted_total=64 ", "accepted_per_sec=", "backpressure_pauses=0 ",
                                "protocol_errors=0"}) &&
                hasMetricsLine(
                    logs, "[reader-metrics] ",
                    {"consumed_total=64 ", "written_bytes_total=2048 ", "synced_bytes_total=0 ",
                     "unsynced_bytes=2048 ", "written_bytes_per_sec=", "sync_successes=0 ",
                     "storage_errors=0 ", "write_latency_samples=", "sync_latency_samples=0 ",
                     "sync_p50_us=0 ", "sync_p95_us=0 ", "sync_p99_us=0 ", "status=running"})) {
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
                                "sync_successes=1 ", "storage_errors=0 ", "status=success"},
                               true),
                "final Reader metrics did not confirm successful sync and valid latency");
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

void runWatchdog(const fs::path &binary)
{
    TempDir temp;
    const auto log = temp.file("watchdog.log");
    const auto data = temp.file("records.bin");
    const auto port = freePort();
    Child server(binary, log, data, port, "process");

    const auto waitFor = [&](std::string_view fields, std::size_t offset,
                             std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            require(server.running(), "Server exited during watchdog test");
            const auto logs = readText(log);
            if (logs.size() >= offset &&
                hasMetricsLine(std::string_view(logs).substr(offset), "[metrics] ", {fields})) {
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
        throw std::runtime_error("watchdog state not observed: " + std::string(fields));
    };

    try {
        waitFor("reader_health=responsive", 0, 5s);
        // An idle but scheduled Reader must remain responsive beyond the threshold.
        const auto idleUntil = Clock::now() + 3500ms;
        while (Clock::now() < idleUntil) {
            require(server.running(), "Server exited during idle observation");
            require(readText(log).find("reader_health=suspected_stall") == std::string::npos,
                    "idle Reader falsely classified as stalled");
            std::this_thread::sleep_for(50ms);
        }

        const auto children = fs::path("/proc") / std::to_string(server.pid()) / "task" /
                              std::to_string(server.pid()) / "children";
        std::ifstream input(children);
        pid_t reader = 0;
        input >> reader;
        require(reader > 0, "cannot locate Reader PID");
        require(kill(reader, SIGSTOP) == 0, "cannot stop Reader");
        const auto stopOffset = readText(log).size();
        sendFrames(port);
        waitFor("reader_health=suspected_stall", stopOffset, 7s);
        require(server.running(), "diagnostic watchdog must not kill Server");

        const auto resumeOffset = readText(log).size();
        require(kill(reader, SIGCONT) == 0, "cannot resume Reader");
        waitFor("reader_health=responsive", resumeOffset, 5s);
        server.stop();
        require(fs::file_size(data) == 64 * telemetry::protocol::kFrameSize,
                "Reader did not persist queued frames after recovery");
        std::cout << "watchdog idle/stall/recovery PASS\n";
    } catch (...) {
        std::cerr << readText(log) << '\n';
        throw;
    }
}

void runWriteFailure(const fs::path &binary, std::string_view mode)
{
    TempDir temp;
    const auto data = temp.file("limited.bin");
    const auto port = freePort();
    Child server(binary, temp.file("unused.log"), data, port, mode, true);
    try {
        const auto deadline = Clock::now() + 5s;
        bool ready = false;
        while (Clock::now() < deadline) {
            require(server.running(), "limited Server exited during startup");
            server.drainLog();
            const auto &logs = server.capturedLog();
            if (logs.find("[server] listening on") != std::string::npos &&
                logs.find("[reader] reading telemetry from shared memory") != std::string::npos) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(20ms);
        }
        require(ready, "limited Server startup timeout");
        sendFrames(port, 1);
        server.waitFailure();
        require(hasMetricsLine(server.capturedLog(), "[reader-metrics] ",
                               {"consumed_total=1 ", "written_bytes_total=16 ",
                                "synced_bytes_total=0 ", "unsynced_bytes=16 ",
                                "storage_errors=1 ", "write_latency_samples=1 ",
                                "sync_latency_samples=0 ", "status=failure"}),
                "partial write failure metrics missing or incorrect");
        require(fs::file_size(data) == 16, "expected partial frame on disk");
        std::cout << "write failure mode=" << mode << " PASS\n";
    } catch (...) {
        server.drainLog();
        std::cerr << server.capturedLog() << '\n';
        throw;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        require(argc == 3 || argc == 4,
                "usage: telemetry-monitoring-test SERVER process|thread [watchdog|write-failure]");
        const fs::path binary = fs::absolute(argv[1]);
        const std::string_view mode = argv[2];
        require(mode == "process" || mode == "thread", "unknown reader mode");
        if (argc == 3) {
            run(binary, mode);
        } else if (std::string_view(argv[3]) == "watchdog" && mode == "process") {
            runWatchdog(binary);
        } else if (std::string_view(argv[3]) == "write-failure") {
            runWriteFailure(binary, mode);
        } else {
            throw std::runtime_error("unknown monitoring scenario");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "monitoring integration FAILED: " << error.what() << '\n';
        return 1;
    }
}
