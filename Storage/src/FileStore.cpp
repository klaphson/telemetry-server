#include "FileStore.hpp"
#include "TelemetryCodec.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>

FileStore::FileStore(FileStoreConfig config) : m_config(config) {}

FileStore::~FileStore()
{
    if (m_fd != -1) {
        close(m_fd);
    }
    if (m_directoryFd != -1) {
        close(m_directoryFd);
    }
}

bool FileStore::openFile(std::string_view path)
{
    if (m_fd != -1 || m_directoryFd != -1) {

        std::cerr << "[storage] file already open\n";

        return false;
    }

    if (!validateConfig()) {
        return false;
    }

    if (!openDirectory(path)) {
        return false;
    }

    if (!openActiveFile()) {
        return false;
    }

    std::cout << "[storage] opened " << m_directoryPath << '/' << m_fileName << '\n';

    return true;
}

bool FileStore::appendFrame(std::span<const std::byte> frame)
{
    using telemetry::protocol::kFrameSize;

    if (m_fd == -1) {
        return false;
    }

    if (frame.size() != kFrameSize) {
        std::cerr << "[storage] invalid frame size=" << frame.size() << '\n';

        return false;
    }

    if (m_currentSize > m_config.maxSegmentBytes - frame.size()) {

        if (!rotate()) {
            return false;
        }
    }

    std::size_t offset = 0;

    while (offset < frame.size()) {
        const ssize_t written = write(m_fd, frame.data() + offset, frame.size() - offset);

        if (written > 0) {
            offset += static_cast<std::size_t>(written);

            continue;
        }

        if (written == -1 && errno == EINTR) {
            continue;
        }

        perror("write telemetry file");
        return false;
    }

    m_currentSize += frame.size();
    m_unsyncedBytes += frame.size();

    if (m_config.syncEveryBytes != 0 && m_unsyncedBytes >= m_config.syncEveryBytes) {

        if (!sync()) {
            return false;
        }
    }

    return true;
}

bool FileStore::sync()
{
    if (m_fd == -1) {
        return false;
    }

    while (true) {
        if (fdatasync(m_fd) == 0) {
            m_unsyncedBytes = 0;
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        perror("fdatasync telemetry file");
        return false;
    }
}

bool FileStore::validateConfig() const
{
    using telemetry::protocol::kFrameSize;

    if (m_config.maxSegmentBytes < kFrameSize) {
        std::cerr << "[storage] segment size smaller than frame\n";

        return false;
    }

    if (m_config.maxSegmentBytes % kFrameSize != 0) {

        std::cerr << "[storage] segment size must be " << "a multiple of frame size\n";

        return false;
    }

    if (m_config.syncEveryBytes != 0 && m_config.syncEveryBytes > m_config.maxSegmentBytes) {

        std::cerr << "[storage] sync interval exceeds " << "segment size\n";

        return false;
    }

    return true;
}

bool FileStore::openDirectory(std::string_view path)
{
    const std::filesystem::path fullPath{std::string{path}};

    const auto fileName = fullPath.filename();

    if (fileName.empty()) {
        std::cerr << "[storage] invalid data file path\n";

        return false;
    }

    auto parent = fullPath.parent_path();

    if (parent.empty()) {
        parent = ".";
    }

    m_directoryPath = parent.string();

    m_fileName = fileName.string();

    m_directoryFd = open(m_directoryPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (m_directoryFd == -1) {
        perror("open storage directory");
        return false;
    }

    return true;
}

bool FileStore::openActiveFile()
{
    bool created = false;

    m_fd = openat(m_directoryFd, m_fileName.c_str(),
                  O_WRONLY | O_APPEND | O_CLOEXEC | O_CREAT | O_EXCL, 0640);

    if (m_fd >= 0) {
        created = true;
    } else if (errno == EEXIST) {
        m_fd = openat(m_directoryFd, m_fileName.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);

        if (m_fd == -1) {
            perror("open existing telemetry file");
            return false;
        }
    } else {
        perror("create telemetry file");
        return false;
    }

    if (created) {
        if (!syncDirectory()) {
            return false;
        }
    }

    if (!recoverPartialFrame()) {
        return false;
    }

    return true;
}

bool FileStore::recoverPartialFrame()
{
    using telemetry::protocol::kFrameSize;

    struct stat status {
    };

    if (fstat(m_fd, &status) == -1) {
        perror("fstat telemetry file");
        return false;
    }

    if (!S_ISREG(status.st_mode)) {
        std::cerr << "[storage] data path is not " << "a regular file\n";

        return false;
    }

    if (status.st_size < 0) {
        return false;
    }

    const auto frameSize = static_cast<off_t>(kFrameSize);

    const off_t remainder = status.st_size % frameSize;

    off_t validSize = status.st_size;

    if (remainder != 0) {
        validSize -= remainder;

        std::cerr << "[storage] truncating partial tail: " << remainder << " bytes\n";

        if (ftruncate(m_fd, validSize) == -1) {
            perror("ftruncate telemetry file");
            return false;
        }

        if (!sync()) {
            return false;
        }
    }

    m_currentSize = static_cast<std::size_t>(validSize);

    return true;
}

bool FileStore::rotate()
{
    if (m_fd == -1) {
        return false;
    }

    if (!sync()) {
        return false;
    }

    if (!closeActiveFile()) {
        return false;
    }

    const std::string archiveName = makeArchiveName();

    if (archiveName.empty()) {
        return false;
    }

    if (renameat(m_directoryFd, m_fileName.c_str(), m_directoryFd, archiveName.c_str()) == -1) {

        perror("rename telemetry segment");
        return false;
    }

    if (!syncDirectory()) {
        return false;
    }

    m_currentSize = 0;
    m_unsyncedBytes = 0;

    if (!openActiveFile()) {
        return false;
    }

    std::cout << "[storage] rotated " << m_fileName << " -> " << archiveName << '\n';

    return true;
}

bool FileStore::syncDirectory() const
{
    if (m_directoryFd == -1) {
        return false;
    }

    while (true) {
        if (fsync(m_directoryFd) == 0) {
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        perror("fsync storage directory");
        return false;
    }
}

std::string FileStore::makeArchiveName()
{
    timespec time{};

    if (clock_gettime(CLOCK_REALTIME, &time) == -1) {
        perror("clock_gettime");
        return {};
    }

    return m_fileName + "." + std::to_string(time.tv_sec) + "." + std::to_string(time.tv_nsec) +
           "." + std::to_string(m_rotationSequence++) + ".segment";
}

bool FileStore::closeActiveFile()
{
    if (m_fd == -1) {
        return true;
    }

    const int fd = m_fd;
    m_fd = -1;

    if (close(fd) == -1) {
        perror("close telemetry file");
        return false;
    }

    return true;
}