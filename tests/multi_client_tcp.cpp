// Bounded C++20 TCP integration tests for process and threaded Reader modes.
// Production Server/Reader sources are not modified by this executable.
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
#include <iterator>
#include <iostream>
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

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using telemetry::protocol::Frame;
using telemetry::protocol::TelemetryRecord;
using telemetry::protocol::encode;
using telemetry::protocol::kFrameSize;
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
    [[nodiscard]] int get() const { return m_fd; }

  private:
    int m_fd;
};

class TemporaryDirectory
{
  public:
    explicit TemporaryDirectory(std::string_view prefix)
    {
        std::string pattern = (fs::temp_directory_path() / (std::string(prefix) + "XXXXXX")).string();
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
    [[nodiscard]] const fs::path &path() const { return m_path; }

  private:
    fs::path m_path;
};

Frame record(std::uint32_t sensor, std::uint64_t sequence)
{
    return encode(TelemetryRecord{.sensorId = sensor, .metricId = 7,
                                  .timestampNs = sequence, .value = static_cast<double>(sensor)});
}

std::vector<char> readFile(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail("cannot open file " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void writeFile(const fs::path &path, std::span<const Frame> frames)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(file), "cannot write test data file");
    for (const Frame &frame : frames) {
        file.write(reinterpret_cast<const char*>(frame.data()),
                   static_cast<std::streamsize>(frame.size()));
    }
    require(static_cast<bool>(file), "failed writing test data");
}

std::size_t verifyRecords(const fs::path &path,
                          const std::map<std::uint32_t, std::uint64_t> &expected)
{
    const auto data = readFile(path);
    std::uint64_t expectedFrames = 0;
    for (const auto &[sensor, count] : expected) {
        (void)sensor;
        expectedFrames += count;
    }
    require(data.size() == expectedFrames * kFrameSize,
            "wrong file size: expected " + std::to_string(expectedFrames * kFrameSize) +
                ", got " + std::to_string(data.size()));

    std::map<std::uint32_t, std::uint64_t> positions;
    for (std::size_t offset = 0; offset < data.size(); offset += kFrameSize) {
        const auto* const bytes = reinterpret_cast<const std::byte*>(data.data() + offset);
        const auto decoded = telemetry::protocol::decode({bytes, kFrameSize});
        require(static_cast<bool>(decoded), "invalid protocol frame at byte " + std::to_string(offset));
        const auto sensor = decoded.record.sensorId;
        require(expected.contains(sensor), "unexpected sensor " + std::to_string(sensor));
        const std::uint64_t next = ++positions[sensor];
        const Frame correct = record(sensor, next);
        require(std::memcmp(bytes, correct.data(), kFrameSize) == 0,
                "corrupt, duplicate or out-of-order frame for sensor " +
                    std::to_string(sensor) + " at sequence " + std::to_string(next));
    }
    for (const auto &[sensor, count] : expected) {
        require(positions[sensor] == count, "missing frames for sensor " + std::to_string(sensor));
    }
    return static_cast<std::size_t>(expectedFrames);
}

void verifierSelfTest()
{
    TemporaryDirectory directory("telemetry-multi-self-");
    const fs::path path = directory.path() / "records.bin";
    const std::array good{record(101, 1), record(102, 1), record(101, 2)};
    const std::map<std::uint32_t, std::uint64_t> expected{{101, 2}, {102, 1}};
    writeFile(path, good);
    require(verifyRecords(path, expected) == 3, "valid interleaved records were rejected");

    const auto rejects = [&](std::span<const Frame> frames) {
        writeFile(path, frames);
        try {
            (void)verifyRecords(path, expected);
        } catch (const std::runtime_error &) {
            return;
        }
        fail("verifier accepted damaged records");
    };
    rejects(std::span{good}.first(2));
    const std::array duplicate{good[0], good[1], good[1]};
    rejects(duplicate);
    const std::array reordered{good[2], good[1], good[0]};
    rejects(reordered);
    const std::array mutated{good[0], good[1], record(101, 3)};
    rejects(mutated);

    writeFile(path, good);
    fs::resize_file(path, 2 * kFrameSize + 17);
    try {
        (void)verifyRecords(path, expected);
        fail("verifier accepted truncated last frame");
    } catch (const std::runtime_error &error) {
        require(std::string_view(error.what()).find("wrong file size") != std::string_view::npos,
                "unexpected self-test error");
    }
    std::cout << "multi-client C++20 verifier self-test: PASS\n";
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

void sendFrame(int socketFd, const Frame &frame)
{
    sendAll(socketFd, frame);
}

enum class Behavior { Fast, Slow, Fragmented, Partial, Invalid, Idle };
struct ClientSpec {
    std::uint32_t sensor;
    unsigned count;
    Behavior behavior;
};

void sendClient(int socketFd, ClientSpec spec)
{
    if (spec.behavior == Behavior::Idle) {
        std::this_thread::sleep_for(150ms);
    }
    for (unsigned first = 1; first <= spec.count; first += 64) {
        const unsigned n = std::min(64U, spec.count - first + 1);
        if (spec.behavior == Behavior::Slow || spec.behavior == Behavior::Fragmented) {
            for (unsigned i = 0; i < n; ++i) {
                const Frame frame = record(spec.sensor, first + i);
                if (spec.behavior == Behavior::Fragmented) {
                    sendAll(socketFd, std::span{frame}.first(7));
                    std::this_thread::sleep_for(500us);
                    sendAll(socketFd, std::span{frame}.subspan(7, 12));
                    sendAll(socketFd, std::span{frame}.subspan(19));
                } else {
                    sendFrame(socketFd, frame);
                    std::this_thread::sleep_for(3ms);
                }
            }
        } else {
            std::array<Frame, 64> frames{};
            for (unsigned i = 0; i < n; ++i) {
                frames[i] = record(spec.sensor, first + i);
            }
            sendAll(socketFd, std::as_bytes(std::span{frames}.first(n)));
        }
    }
    if (spec.behavior == Behavior::Partial) {
        const Frame last = record(spec.sensor, spec.count + 1);
        sendAll(socketFd, std::span{last}.first(17));
    } else if (spec.behavior == Behavior::Invalid) {
        sendFrame(socketFd, Frame{});
    }
}

void sendConcurrent(std::uint16_t port, const std::vector<ClientSpec> &specs)
{
    // Prepare all connections before creating workers so a failed connect
    // cannot strand any worker indefinitely at the start barrier.
    std::vector<Descriptor> connections;
    connections.reserve(specs.size());
    for (std::size_t i = 0; i < specs.size(); ++i) {
        connections.emplace_back(connectClient(port));
    }
    std::barrier gate(static_cast<std::ptrdiff_t>(specs.size()));
    std::vector<std::exception_ptr> errors(specs.size());
    std::vector<std::jthread> workers;
    workers.reserve(specs.size());
    for (std::size_t index = 0; index < specs.size(); ++index) {
        workers.emplace_back([&, index] {
            gate.arrive_and_wait();
            try {
                sendClient(connections[index].get(), specs[index]);
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    for (const auto &error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
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
                while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
    }
    ServerProcess(const ServerProcess &) = delete;
    ServerProcess &operator=(const ServerProcess &) = delete;
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

bool fileContainsFrame(const fs::path &path, const Frame &frame)
{
    std::error_code error;
    if (!fs::exists(path, error) || error) {
        return false;
    }
    const auto data = readFile(path);
    for (std::size_t i = 0; i + kFrameSize <= data.size(); i += kFrameSize) {
        if (std::memcmp(data.data() + i, frame.data(), kFrameSize) == 0) {
            return true;
        }
    }
    return false;
}

void runMode(const fs::path &binary, std::string_view mode)
{
    TemporaryDirectory directory("telemetry-multi-");
    const auto logPath = directory.path() / "server.log";
    const auto dataPath = directory.path() / "records.bin";
    const std::uint16_t port = allocatePort();
    try {
        ServerProcess server(binary, mode, port, dataPath, logPath);
        server.waitReady();

        const std::vector<ClientSpec> clients{
            {101, 1024, Behavior::Fast}, {102, 1024, Behavior::Fast},
            {103, 1024, Behavior::Fast}, {201, 24, Behavior::Slow},
            {202, 16, Behavior::Fragmented}, {203, 32, Behavior::Partial},
            {204, 8, Behavior::Invalid}, {205, 3, Behavior::Idle}};
        sendConcurrent(port, clients);
        std::map<std::uint32_t, std::uint64_t> expected;
        for (const auto &client : clients) {
            expected[client.sensor] = client.count;
        }
        // A bad client must not prevent a subsequent valid connection.
        auto reconnect = connectClient(port);
        sendClient(reconnect.get(), {206, 10, Behavior::Fast});
        expected[206] = 10;

        std::atomic<std::uint64_t> hotSent{0};
        std::exception_ptr hotError;
        std::jthread hot([&](std::stop_token stop) {
            try {
                auto sock = connectClient(port);
                while (!stop.stop_requested()) {
                    const std::uint64_t base = hotSent.load(std::memory_order_relaxed);
                    std::array<Frame, 128> frames{};
                    for (std::size_t i = 0; i < frames.size(); ++i) {
                        frames[i] = record(301, base + i + 1);
                    }
                    sendAll(sock.get(), std::as_bytes(std::span{frames}));
                    hotSent.store(base + frames.size(), std::memory_order_release);
                    std::this_thread::sleep_for(500us);
                }
            } catch (...) {
                hotError = std::current_exception();
            }
        });
        try {
            const auto started = Clock::now() + 5s;
            while (hotSent.load(std::memory_order_acquire) == 0 && Clock::now() < started) {
                require(server.running(), "server died while hot client was connecting");
                std::this_thread::sleep_for(10ms);
            }
            require(hotSent.load(std::memory_order_acquire) > 0, "hot sender did not start");
            const Frame probe = record(302, 1);
            {
                auto sock = connectClient(port);
                sendAll(sock.get(), std::span{probe}.first(11));
                std::this_thread::sleep_for(2ms);
                sendAll(sock.get(), std::span{probe}.subspan(11));
            }
            const auto probeDeadline = Clock::now() + 8s;
            while (Clock::now() < probeDeadline && !fileContainsFrame(dataPath, probe)) {
                require(server.running(), "server died during slow probe");
                std::this_thread::sleep_for(20ms);
            }
            require(fileContainsFrame(dataPath, probe),
                    "probe was not persisted while the hot client was active");
            const std::uint64_t progress = hotSent.load(std::memory_order_acquire);
            const auto progressDeadline = Clock::now() + 3s;
            while (hotSent.load(std::memory_order_acquire) < progress + 256 &&
                   Clock::now() < progressDeadline) {
                std::this_thread::sleep_for(10ms);
            }
            require(hotSent.load(std::memory_order_acquire) >= progress + 256,
                    "hot client stopped progressing");
        } catch (...) {
            hot.request_stop();
            hot.join();
            throw;
        }
        hot.request_stop();
        hot.join();
        if (hotError) {
            std::rethrow_exception(hotError);
        }
        const std::uint64_t hotCount = hotSent.load(std::memory_order_relaxed);
        expected[301] = hotCount;
        expected[302] = 1;

        std::uint64_t total = 0;
        for (const auto &[sensor, count] : expected) {
            (void)sensor;
            total += count;
        }
        waitForFile(dataPath, total * kFrameSize, server);
        server.stopGracefully();
        const std::string logs = logContents(logPath);
        require(logs.find("[reader] EOF") != std::string::npos, "Reader did not log EOF");
        require(logs.find("protocol error") != std::string::npos,
                "malformed client was not rejected");
        const auto verified = verifyRecords(dataPath, expected);
        std::cout << "mode=" << mode << " clients=" << expected.size()
                  << " frames=" << verified << " hot_frames=" << hotCount
                  << " verified=PASS\n";
    } catch (...) {
        try {
            std::cerr << "Server log (" << mode << "):\n" << logContents(logPath) << '\n';
        } catch (...) {}
        throw;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            verifierSelfTest();
            return 0;
        }
        if (argc != 4 || std::string_view(argv[2]) != "--mode") {
            std::cerr << "Usage: telemetry-multi-client-test [--self-test | SERVER --mode process|thread]\n";
            return 2;
        }
        const fs::path binary = fs::absolute(argv[1]);
        require(fs::is_regular_file(binary), "compiled Server binary not found");
        const std::string_view mode = argv[3];
        require(mode == "process" || mode == "thread", "invalid Reader mode");
        runMode(binary, mode);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "multi-client C++20 integration test FAILED: " << error.what() << '\n';
        return 1;
    }
}
