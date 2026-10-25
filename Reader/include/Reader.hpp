#ifndef READER_HPP
#define READER_HPP

#include <cstddef>
#include <string>

class SharedRingBuffer;

struct ReaderConfig {
    std::string dataFilePath;
    std::size_t maxSegmentBytes;
    std::size_t syncEveryBytes;
    bool quiet = false;
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

    // Borrows the ring and eventfds. The producer closes the ring and notifies
    // dataReadyFd to let this call drain, sync, and return successfully.
    int run(SharedRingBuffer &ring, int dataReadyFd, int spaceReadyFd,
            const ReaderConfig &config) const;
};

#endif
