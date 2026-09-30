#ifndef FILE_STORE_HPP
#define FILE_STORE_HPP

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

    [[nodiscard]]
    bool sync();

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

    int m_fd = -1;
    int m_directoryFd = -1;

    std::string m_directoryPath;
    std::string m_fileName;

    std::size_t m_currentSize = 0;
    std::size_t m_unsyncedBytes = 0;

    std::uint64_t m_rotationSequence = 0;
};

#endif