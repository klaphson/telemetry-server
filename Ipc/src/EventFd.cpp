#include "EventFd.hpp"

#include <sys/eventfd.h>

#include <cerrno>
#include <cstdio>

namespace telemetry::ipc
{

bool notifyEvent(int fd) noexcept
{
    while (eventfd_write(fd, 1) == -1) {
        if (errno == EINTR) {
            continue;
        }

        perror("eventfd_write");
        return false;
    }

    return true;
}

bool consumeEvent(int fd) noexcept
{
    eventfd_t value{};

    while (eventfd_read(fd, &value) == -1) {
        if (errno == EINTR) {
            continue;
        }

        perror("eventfd_read");
        return false;
    }

    return true;
}

} // namespace telemetry::ipc