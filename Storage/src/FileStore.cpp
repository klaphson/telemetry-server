#include "FileStore.hpp"
#include "TelemetryCodec.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <iostream>
#include <string>

FileStore::~FileStore()
{
    if (m_fd != -1) {
        close(m_fd);
    }
}

bool FileStore::openFile(std::string_view path)
{
    if (m_fd != -1) {
        std::cerr << "[storage] file already open\n";

        return false;
    }

    const std::string pathString{path};

    m_fd = open(pathString.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);

    if (m_fd == -1) {
        perror("open telemetry file");
        return false;
    }

    if (!recoverPartialFrame()) {
        close(m_fd);
        m_fd = -1;

        return false;
    }

    std::cout << "[storage] opened " << pathString << '\n';

    return true;
}

bool FileStore::appendFrame(std::span<const std::byte> frame) const
{
    using telemetry::protocol::kFrameSize;

    if (m_fd == -1) {
        std::cerr << "[storage] append on closed file\n";

        return false;
    }

    if (frame.size() != kFrameSize) {
        std::cerr << "[storage] invalid frame size=" << frame.size() << '\n';

        return false;
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

    return true;
}

bool FileStore::sync() const
{
    if (m_fd == -1) {
        return false;
    }

    while (true) {
        if (fdatasync(m_fd) == 0) {
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        perror("fdatasync telemetry file");
        return false;
    }
}

bool FileStore::recoverPartialFrame() const
{
    using telemetry::protocol::kFrameSize;

    struct stat status {
    };

    if (fstat(m_fd, &status) == -1) {
        perror("fstat telemetry file");
        return false;
    }

    if (status.st_size < 0) {
        std::cerr << "[storage] invalid negative file size\n";

        return false;
    }

    const auto frameSize = static_cast<off_t>(kFrameSize);

    const off_t remainder = status.st_size % frameSize;

    if (remainder == 0) {
        return true;
    }

    const off_t validSize = status.st_size - remainder;

    std::cerr << "[storage] truncating partial tail: " << remainder << " bytes\n";

    if (ftruncate(m_fd, validSize) == -1) {
        perror("ftruncate telemetry file");
        return false;
    }

    return true;
}