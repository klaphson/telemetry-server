#ifndef IPC_QUEUE_HPP
#define IPC_QUEUE_HPP

#include <cstddef>
#include <string>

struct IpcQueue {
    static constexpr std::size_t maxBufferSize = 4 * 1024 * 1024;
    static constexpr std::size_t compactionThreshold = 64 * 1024;

    std::string buffer;
    std::size_t offset = 0;

    [[nodiscard]] bool empty() const noexcept
    {
        return offset >= buffer.size();
    }

    [[nodiscard]] std::size_t pendingBytes() const noexcept
    {
        return empty() ? 0 : buffer.size() - offset;
    }

    void compactIfNeeded()
    {
        if (offset == 0) {
            return;
        }

        if (offset == buffer.size()) {
            buffer.clear();
            offset = 0;
            return;
        }

        if (offset >= compactionThreshold) {
            buffer.erase(0, offset);
            offset = 0;
        }
    }

    bool appendLine(const std::string &line)
    {
        compactIfNeeded();

        const std::size_t required = line.size() + 1;
        if (pendingBytes() + required > maxBufferSize) {
            return false;
        }

        buffer.append(line);
        buffer.push_back('\n');
        return true;
    }
};

#endif // IPC_QUEUE_HPP
