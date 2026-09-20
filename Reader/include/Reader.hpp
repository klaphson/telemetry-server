#ifndef READER_HPP
#define READER_HPP

#include <array>
#include <string>

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
    int run(int pipeReadFd) const;

  private:
    int m_pipeReadFd;
    constexpr static std::size_t bufferSize = 4096;
    void readLine(std::string &pending, const std::array<char, bufferSize> &buffer,
                  ssize_t bytesRead) const;
};

#endif // READER_HPP
