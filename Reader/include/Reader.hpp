#ifndef READER_HPP
#define READER_HPP

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class FileStore;

struct ReaderConfig {
    std::string dataFilePath;

    std::size_t maxSegmentBytes;
    std::size_t syncEveryBytes;
};

class Reader
{
  public:
    Reader() = default;
    ~Reader() = default;

    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    Reader(Reader &&) = delete;
    Reader &operator=(Reader &&) = delete;

    // Borrows the descriptor; the caller closes it after run() returns.
    int run(int pipeReadFd, const ReaderConfig &config) const;

  private:
    bool readFrames(std::span<const std::byte> bytes, std::vector<std::byte> &pending,
                    std::size_t &offset, FileStore &store) const;

    constexpr static std::size_t bufferSize = 4096;
};

#endif // READER_HPP
