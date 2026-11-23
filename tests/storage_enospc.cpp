// Linux integration test: genuine ENOSPC on an isolated, size-limited tmpfs.
// Never fill a shared filesystem, /dev/shm, or the regular development volume.
#include "FileStore.hpp"
#include "TelemetryCodec.hpp"

#include <fcntl.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using telemetry::protocol::encode;
using telemetry::protocol::Frame;
using telemetry::protocol::kFrameSize;
using telemetry::protocol::TelemetryRecord;

struct Skipped : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string errorText(std::string_view action)
{
    return std::string(action) + ": " + std::strerror(errno);
}

class Descriptor
{
  public:
    explicit Descriptor(int fd) : m_fd(fd)
    {
        require(fd >= 0, errorText("open"));
    }
    ~Descriptor() { close(m_fd); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    [[nodiscard]] int get() const { return m_fd; }

  private:
    int m_fd;
};

class TemporaryDirectory
{
  public:
    TemporaryDirectory()
    {
        std::string path = (fs::temp_directory_path() / "telemetry-enospc-XXXXXX").string();
        require(mkdtemp(path.data()) != nullptr, errorText("mkdtemp"));
        m_path = path;
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    [[nodiscard]] const fs::path &path() const { return m_path; }

  private:
    fs::path m_path;
};

// Mount namespaces are private to this test process. Destroy the mount
// before its TemporaryDirectory; any failure path unwinds both RAII objects.
class IsolatedTmpfs
{
  public:
    explicit IsolatedTmpfs(const fs::path &path) : m_path(path)
    {
        const uid_t outerUid = geteuid();
        const gid_t outerGid = getegid();
        if (unshare(CLONE_NEWUSER) == -1) {
            throw Skipped(errorText("unshare user namespace"));
        }
        const auto writeProc = [](const char* name, const std::string &value) {
            const int fd = open(name, O_WRONLY | O_CLOEXEC);
            if (fd < 0) {
                throw Skipped(errorText(name));
            }
            const ssize_t count = write(fd, value.data(), value.size());
            const int error = errno;
            close(fd);
            if (count != static_cast<ssize_t>(value.size())) {
                errno = error;
                throw Skipped(errorText(name));
            }
        };
        // Map the invoking user to uid/gid 0 inside the disposable userns.
        // Without the maps, freshly mounted tmpfs returns EOVERFLOW on open().
        writeProc("/proc/self/setgroups", "deny\n");
        writeProc("/proc/self/uid_map", "0 " + std::to_string(outerUid) + " 1\n");
        writeProc("/proc/self/gid_map", "0 " + std::to_string(outerGid) + " 1\n");
        if (setresgid(0, 0, 0) == -1 || setresuid(0, 0, 0) == -1) {
            throw Skipped(errorText("switch to mapped uid/gid"));
        }
        if (unshare(CLONE_NEWNS) == -1) {
            throw Skipped(errorText("unshare mount namespace"));
        }
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == -1) {
            throw Skipped(errorText("make mount propagation private"));
        }
        if (mount("tmpfs", path.c_str(), "tmpfs", MS_NODEV | MS_NOSUID,
                  "size=524288,mode=0700") == -1) {
            throw Skipped(errorText("mount isolated 512 KiB tmpfs"));
        }
        m_mounted = true;
    }
    ~IsolatedTmpfs()
    {
        if (m_mounted && umount2(m_path.c_str(), MNT_DETACH) == -1) {
            std::cerr << "[enospc] umount failed: " << std::strerror(errno) << '\n';
        }
    }
    IsolatedTmpfs(const IsolatedTmpfs &) = delete;
    IsolatedTmpfs &operator=(const IsolatedTmpfs &) = delete;

  private:
    fs::path m_path;
    bool m_mounted = false;
};

// An explicit externally provided directory must itself be an empty,
// dedicated tmpfs mount with a tightly limited capacity. Never accept
// /dev/shm, /tmp, a bind mount of the ordinary data volume, or large tmpfs.
void validateExternalMount(const fs::path &path)
{
    require(fs::is_directory(path), "ENOSPC directory is missing");
    require(fs::is_empty(path), "ENOSPC directory must start empty");
    struct stat directory{};
    struct stat parent{};
    require(stat(path.c_str(), &directory) == 0, errorText("stat ENOSPC directory"));
    require(stat(path.parent_path().c_str(), &parent) == 0, errorText("stat mount parent"));
    require(directory.st_dev != parent.st_dev,
            "ENOSPC directory must be a separate mount point");
    struct statfs filesystem{};
    require(statfs(path.c_str(), &filesystem) == 0, errorText("statfs ENOSPC mount"));
    require(filesystem.f_type == TMPFS_MAGIC, "ENOSPC mount must be tmpfs");
    struct statvfs volume{};
    require(statvfs(path.c_str(), &volume) == 0, errorText("statvfs ENOSPC mount"));
    const auto bytes = static_cast<std::uint64_t>(volume.f_blocks) * volume.f_frsize;
    require(bytes > 0 && bytes <= 1024 * 1024,
            "ENOSPC tmpfs must be size-limited to at most 1 MiB");
}

Frame frame(std::uint64_t sequence)
{
    return encode(TelemetryRecord{.sensorId = 17,
                                  .metricId = 4,
                                  .timestampNs = sequence,
                                  .value = static_cast<double>(sequence)});
}

// Fill only the private 512 KiB tmpfs; the filler is released before restart.
void exhaustSpace(const fs::path &path)
{
    Descriptor filler(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    const std::array<std::byte, 4096> page{};
    for (int attempts = 0; attempts < 512; ++attempts) {
        const ssize_t written = write(filler.get(), page.data(), page.size());
        if (written > 0) {
            continue;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written == -1 && errno == ENOSPC) {
            return;
        }
        throw std::runtime_error(errorText("filling dedicated tmpfs"));
    }
    throw std::runtime_error("tmpfs capacity was not exhausted within limit");
}

void exerciseEnospc(const fs::path &directory)
{
    const auto dataPath = directory / "telemetry.bin";
    const auto fillerPath = directory / "filler.bin";
    const FileStoreConfig config{.maxSegmentBytes = 1024 * 1024, .syncEveryBytes = 0};
    constexpr std::size_t batchFrames = 8192;
    std::array<Frame, batchFrames> batch{};
    for (std::size_t index = 0; index < batch.size(); ++index) {
        batch[index] = frame(index + 2);
    }

    {
        FileStore store{config};
        require(store.openFile(dataPath.string()), "cannot create active file on tmpfs");
        require(store.appendFrame(frame(1)), "cannot append initial frame");
        require(store.sync(), "cannot sync initial frame");

        exhaustSpace(fillerPath);
        const bool appended = store.appendFrames(std::as_bytes(std::span{batch}));
        // The filler already observed a real ENOSPC. FileStore reports the
        // failing write via a bool; it does not guarantee preserving errno.
        require(!appended, "FileStore silently accepted a write on a full filesystem");
    }

    const auto sizeBeforeRecovery = fs::file_size(dataPath);
    require(sizeBeforeRecovery >= kFrameSize, "initial frame was lost");
    require(sizeBeforeRecovery < (batchFrames + 1) * kFrameSize,
            "full batch unexpectedly persisted despite ENOSPC");

    // Reclaim only our own filler, then restart storage. Incomplete trailing
    // bytes may be removed; every preceding complete frame must be preserved.
    require(fs::remove(fillerPath), "could not release filler bytes");
    const std::uint64_t survivingFrames = sizeBeforeRecovery / kFrameSize;
    {
        FileStore restarted{config};
        require(restarted.openFile(dataPath.string()), "cannot reopen file after ENOSPC");
        require(fs::file_size(dataPath) == survivingFrames * kFrameSize,
                "FileStore did not discard the incomplete tail");
        require(restarted.appendFrame(frame(survivingFrames + 1)),
                "append after ENOSPC recovery failed");
        require(restarted.sync(), "final sync after recovery failed");
    }

    const auto dataSize = fs::file_size(dataPath);
    require(dataSize == (survivingFrames + 1) * kFrameSize,
            "wrong length after ENOSPC recovery");
    Descriptor data(open(dataPath.c_str(), O_RDONLY | O_CLOEXEC));
    for (std::uint64_t number = 1; number <= survivingFrames + 1; ++number) {
        Frame found{};
        auto* bytes = found.data();
        std::size_t received = 0;
        while (received < found.size()) {
            const ssize_t n = read(data.get(), bytes + received, found.size() - received);
            require(n > 0, "could not read a complete recovered frame");
            received += static_cast<std::size_t>(n);
        }
        const Frame expected = frame(number);
        require(found == expected, "recovered frames were corrupted or out of order");
    }
    std::cout << "scenario=enospc surviving_frames=" << survivingFrames << " PASS\n";
}

} // namespace

int main()
{
    try {
        if (const char* external = std::getenv("TELEMETRY_ENOSPC_DIR");
            external != nullptr && external[0] != '\0') {
            const fs::path directory = fs::canonical(external);
            validateExternalMount(directory);
            exerciseEnospc(directory);
        } else {
            TemporaryDirectory directory;
            IsolatedTmpfs mount(directory.path());
            exerciseEnospc(directory.path());
        }
        return EXIT_SUCCESS;
    } catch (const Skipped &reason) {
        std::cout << "SKIP: genuine ENOSPC needs user/mount namespaces or an explicit "
                     "dedicated tmpfs: "
                  << reason.what() << '\n';
        return 77;
    } catch (const std::exception &error) {
        std::cerr << "storage ENOSPC test FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
