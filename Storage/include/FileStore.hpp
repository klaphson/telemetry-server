#ifndef FILE_STORE_HPP
#define FILE_STORE_HPP

#include "LatencyHistogram.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

struct FileStoreConfig {
    static constexpr std::size_t defaultMaxSegmentBytes = 64 * 1024 * 1024;

    static constexpr std::size_t defaultSyncEveryBytes = 1 * 1024 * 1024;

    std::size_t maxSegmentBytes = defaultMaxSegmentBytes;

    std::size_t syncEveryBytes = defaultSyncEveryBytes;
    bool measureLatency = false;
};

// Lifetime counters for new data written by this FileStore instance.
struct FileStoreMetrics {
    std::uint64_t writtenBytes = 0;
    std::uint64_t syncedBytes = 0;
    std::uint64_t successfulSyncs = 0;
};

class FileStore
{
  public:
    explicit FileStore(FileStoreConfig config = {});

    ~FileStore();

    FileStore(const FileStore &) = delete;
    FileStore &operator=(const FileStore &) = delete;
    FileStore(FileStore &&) = delete;
    FileStore &operator=(FileStore &&) = delete;

    [[nodiscard]]
    bool openFile(std::string_view path);

    [[nodiscard]]
    bool appendFrame(std::span<const std::byte> frame);

    // Accept one or more complete 32-byte frames. Split writes at the
    // configured segment boundary and durability interval.
    [[nodiscard]]
    bool appendFrames(std::span<const std::byte> frames);

    [[nodiscard]]
    bool sync();

    [[nodiscard]] FileStoreMetrics metrics() const
    {
        return m_metrics;
    }

    [[nodiscard]] const LatencyHistogram &writeLatency() const noexcept
    {
        return m_writeLatency;
    }
    [[nodiscard]] const LatencyHistogram &syncLatency() const noexcept
    {
        return m_syncLatency;
    }

  private:
    [[nodiscard]]
    bool validateConfig() const;

    [[nodiscard]]
    bool openDirectory(std::string_view path);

    [[nodiscard]]
    bool openActiveFile();

    [[nodiscard]]
    bool recoverPartialFrame();

    [[nodiscard]]
    bool rotate();

    [[nodiscard]]
    bool syncDirectory() const;

    [[nodiscard]]
    std::string makeArchiveName();

    [[nodiscard]]
    bool closeActiveFile();

    FileStoreConfig m_config;
    FileStoreMetrics m_metrics;
    LatencyHistogram m_writeLatency;
    LatencyHistogram m_syncLatency;

    int m_fd = -1;
    int m_directoryFd = -1;

    std::string m_directoryPath;
    std::string m_fileName;

    std::size_t m_currentSize = 0;
    std::size_t m_unsyncedBytes = 0;

    std::uint64_t m_rotationSequence = 0;
};

#endif
