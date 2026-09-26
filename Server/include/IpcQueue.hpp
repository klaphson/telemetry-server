#ifndef IPC_QUEUE_HPP
#define IPC_QUEUE_HPP

#include "Buffer.hpp"

#include <span>

struct IpcQueue : Buffer {
    static constexpr std::size_t maxBufferSize = 4 * 1024 * 1024;
    static constexpr std::size_t compactionThreshold = 64 * 1024;
    static constexpr std::size_t pauseIngressThreshold = 3 * 1024 * 1024;
    static constexpr std::size_t resumeIngressThreshold = 1 * 1024 * 1024;

    static_assert(resumeIngressThreshold < pauseIngressThreshold);

    static_assert(pauseIngressThreshold < maxBufferSize);

    [[nodiscard]]
    bool shouldPauseIngress() const noexcept
    {
        return pendingBytes() >= pauseIngressThreshold;
    }

    [[nodiscard]]
    bool canResumeIngress() const noexcept
    {
        return pendingBytes() <= resumeIngressThreshold;
    }

    void compactIfNeeded()
    {
        compact(compactionThreshold);
    }

    bool appendFrame(std::span<const std::byte> frame)
    {
        compactIfNeeded();

        if (pendingBytes() + frame.size() > maxBufferSize) {
            return false;
        }

        data.insert(data.end(), frame.begin(), frame.end());

        return true;
    }
};

#endif
