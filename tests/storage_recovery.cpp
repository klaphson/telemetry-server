// Stage 27: deterministic Linux storage restart and write-failure scenarios.
#include "FileStore.hpp"
#include "TelemetryCodec.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using telemetry::protocol::encode;
using telemetry::protocol::Frame;
using telemetry::protocol::kFrameSize;
using telemetry::protocol::TelemetryRecord;
using namespace std::chrono_literals;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory
{
  public:
    TemporaryDirectory()
    {
        std::string path = (fs::temp_directory_path() / "telemetry-recovery-XXXXXX").string();
        check(mkdtemp(path.data()) != nullptr, "mkdtemp failed");
        m_path = path;
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
    [[nodiscard]] fs::path file(std::string_view name) const
    {
        return m_path / std::string(name);
    }
    [[nodiscard]] const fs::path &path() const
    {
        return m_path;
    }

  private:
    fs::path m_path;
};

Frame frame(std::uint64_t sequence)
{
    return encode(TelemetryRecord{.sensorId = 7,
                                  .metricId = 3,
                                  .timestampNs = sequence,
                                  .value = static_cast<double>(sequence)});
}

std::vector<std::byte> readBytes(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "cannot read persisted file");
    const std::vector<char> raw{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](char value) { return static_cast<std::byte>(value); });
    return bytes;
}

void expectFrames(const fs::path &path, std::span<const std::uint64_t> sequences)
{
    const auto bytes = readBytes(path);
    check(bytes.size() == sequences.size() * kFrameSize, "unexpected file length");
    for (std::size_t i = 0; i < sequences.size(); ++i) {
        const auto expected = frame(sequences[i]);
        check(std::memcmp(bytes.data() + i * kFrameSize, expected.data(), kFrameSize) == 0,
              "unexpected record bytes or order");
    }
}

// The subprocess calls _exit() to emulate an abrupt process death.
void runChild(int (*function)(const fs::path &), const fs::path &path)
{
    const pid_t pid = fork();
    check(pid >= 0, "fork failed");
    if (pid == 0) {
        try {
            _exit(function(path));
        } catch (...) {
            _exit(99);
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int status = 0;
    while (true) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "fault-injection subprocess failed");
            return;
        }
        check(result == 0 || (result == -1 && errno == EINTR), "waitpid failed");
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            throw std::runtime_error("fault-injection subprocess timed out");
        }
        std::this_thread::sleep_for(10ms);
    }
}

int childWritesPartialTail(const fs::path &path)
{
    FileStore store{
        FileStoreConfig{.maxSegmentBytes = 4 * kFrameSize, .syncEveryBytes = kFrameSize}};
    if (!store.openFile(path.string()) || !store.appendFrame(frame(1)) || !store.sync()) {
        return 1;
    }
    const int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd == -1) {
        return 2;
    }
    const Frame incomplete = frame(2);
    const bool partial = write(fd, incomplete.data(), 11) == 11;
    const bool synced = fsync(fd) == 0;
    close(fd);
    return partial && synced ? 0 : 3;
}

void restartRecovery()
{
    TemporaryDirectory directory;
    const auto path = directory.file("telemetry.bin");
    runChild(childWritesPartialTail, path);
    check(fs::file_size(path) == kFrameSize + 11, "truncated tail was not created");

    {
        FileStore store{
            FileStoreConfig{.maxSegmentBytes = 4 * kFrameSize, .syncEveryBytes = kFrameSize}};
        check(store.openFile(path.string()), "FileStore failed to reopen a partial file");
        check(fs::file_size(path) == kFrameSize, "partial tail was not removed");
        check(store.appendFrame(frame(2)), "append after recovery failed");
        check(store.sync(), "sync after recovery failed");
    }
    const std::array<std::uint64_t, 2> expected{1, 2};
    expectFrames(path, expected);
}

void rotationAcrossRestarts()
{
    TemporaryDirectory directory;
    const auto path = directory.file("telemetry.bin");
    const FileStoreConfig config{.maxSegmentBytes = 2 * kFrameSize, .syncEveryBytes = kFrameSize};
    for (std::uint64_t first = 1; first <= 5; first += 2) {
        FileStore store(config);
        check(store.openFile(path.string()), "rotation reopen failed");
        check(store.appendFrame(frame(first)), "rotation append failed");
        if (first < 5) {
            check(store.appendFrame(frame(first + 1)), "rotation second append failed");
        }
        check(store.sync(), "rotation sync failed");
    }

    const std::array<std::uint64_t, 1> active{5};
    expectFrames(path, active);
    std::vector<std::vector<std::byte>> archives;
    for (const auto &entry : fs::directory_iterator(directory.path())) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("telemetry.bin.") && name.ends_with(".segment")) {
            archives.push_back(readBytes(entry.path()));
        }
    }
    check(archives.size() == 2, "missing rotated archives after restarts");
    const auto expectedArchive = [&](std::uint64_t first) {
        std::vector<std::byte> bytes;
        for (const auto number : {first, first + 1}) {
            const Frame record = frame(number);
            bytes.insert(bytes.end(), record.begin(), record.end());
        }
        return bytes;
    };
    const auto firstArchive = expectedArchive(1);
    const auto secondArchive = expectedArchive(3);
    check(std::find(archives.begin(), archives.end(), firstArchive) != archives.end(),
          "first archive missing or corrupted");
    check(std::find(archives.begin(), archives.end(), secondArchive) != archives.end(),
          "second archive missing or corrupted");
}

int childRunsIntoFileLimit(const fs::path &path)
{
    FileStore store{FileStoreConfig{.maxSegmentBytes = 4 * kFrameSize, .syncEveryBytes = 0}};
    if (!store.openFile(path.string())) {
        return 1;
    }
    if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR) {
        return 2;
    }
    const rlimit limit{static_cast<rlim_t>(kFrameSize + 11), static_cast<rlim_t>(kFrameSize + 11)};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        return 3;
    }
    std::array<Frame, 3> records{frame(1), frame(2), frame(3)};
    const auto batch = std::as_bytes(std::span{records});
    // A partial write followed by EFBIG must be reported, not accepted.
    return store.appendFrames(batch) ? 4 : 0;
}

void recoverAfterWriteFailure()
{
    TemporaryDirectory directory;
    const auto path = directory.file("telemetry.bin");
    runChild(childRunsIntoFileLimit, path);
    check(fs::file_size(path) == kFrameSize + 11, "RLIMIT_FSIZE did not produce a truncated frame");
    {
        FileStore store{
            FileStoreConfig{.maxSegmentBytes = 4 * kFrameSize, .syncEveryBytes = kFrameSize}};
        check(store.openFile(path.string()), "recover after write error failed");
        check(fs::file_size(path) == kFrameSize, "write error tail was not truncated");
        check(store.appendFrame(frame(2)), "append after write failure recovery failed");
        check(store.sync(), "sync after write failure recovery failed");
    }
    const std::array<std::uint64_t, 2> expected{1, 2};
    expectFrames(path, expected);
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc != 2) {
            std::cerr << "Usage: telemetry-storage-recovery-test "
                         "restart|rotation|write-failure\n";
            return 2;
        }
        const std::string_view scenario{argv[1]};
        if (scenario == "restart") {
            restartRecovery();
        } else if (scenario == "rotation") {
            rotationAcrossRestarts();
        } else if (scenario == "write-failure") {
            recoverAfterWriteFailure();
        } else {
            throw std::runtime_error("unknown test scenario");
        }
        std::cout << "scenario=" << scenario << " PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "storage recovery test FAILED: " << error.what() << '\n';
        return 1;
    }
}
