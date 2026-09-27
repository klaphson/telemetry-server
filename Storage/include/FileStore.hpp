#ifndef FILE_STORE_HPP
#define FILE_STORE_HPP

#include <cstddef>
#include <span>
#include <string_view>

class FileStore
{
  public:
    FileStore() = default;
    ~FileStore();

    FileStore(const FileStore &) = delete;
    FileStore &operator=(const FileStore &) = delete;
    FileStore(FileStore &&) = delete;
    FileStore &operator=(FileStore &&) = delete;

    [[nodiscard]]
    bool openFile(std::string_view path);

    [[nodiscard]]
    bool appendFrame(std::span<const std::byte> frame) const;

    [[nodiscard]]
    bool sync() const;

  private:
    [[nodiscard]]
    bool recoverPartialFrame() const;

    int m_fd = -1;
};

#endif // FILE_STORE_HPP