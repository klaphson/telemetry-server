#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <iostream>

#include "Reader.hpp"

int Reader::run(int pipeReadFd) const
{
    std::cout << "[reader] pid=" << getpid() << '\n';
    std::cout << "[reader] reading telemetry from pipe\n";

    std::array<char, bufferSize> buffer{};
    std::string pending;

    while (true)
    {
        const ssize_t bytesRead = read(pipeReadFd, buffer.data(), buffer.size());

        if (bytesRead > 0)
        {
            readLine(pending, buffer, bytesRead);
            continue;
        }

        if (bytesRead == 0)
        {
            std::cout << "[reader] EOF\n";
            break;
        }

        if (errno == EINTR)
        {
            continue;
        }

        perror("read");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

void Reader::readLine(std::string &pending, const std::array<char, bufferSize> &buffer, ssize_t bytesRead) const
{
    pending.append(buffer.data(), static_cast<std::size_t>(bytesRead));

    while (true)
    {
        const std::size_t newline = pending.find('\n');
        if (newline == std::string::npos)
        {
            break;
        }

        const std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);

        std::cout << "[reader] telemetry: " << line << '\n';
    }
}