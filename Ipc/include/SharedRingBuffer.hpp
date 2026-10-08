#ifndef SHARED_RING_BUFFER_HPP
#define SHARED_RING_BUFFER_HPP

#include "TelemetryCodec.hpp"

#include <cstddef>
#include <span>

class SharedRingBuffer
{
  public:
    static constexpr std::size_t capacityBytes = 4 * 1024 * 1024;

    static constexpr std::size_t capacityFrames = capacityBytes / telemetry::protocol::kFrameSize;

    static_assert(capacityBytes % telemetry::protocol::kFrameSize == 0);

    SharedRingBuffer() = default;
    ~SharedRingBuffer();

    SharedRingBuffer(const SharedRingBuffer &) = delete;

    SharedRingBuffer &operator=(const SharedRingBuffer &) = delete;

    SharedRingBuffer(SharedRingBuffer &&) = delete;

    SharedRingBuffer &operator=(SharedRingBuffer &&) = delete;

    [[nodiscard]]
    bool create();

    [[nodiscard]]
    bool tryPush(std::span<const std::byte> frame) noexcept;

    [[nodiscard]]
    bool tryPop(std::span<std::byte> frame) noexcept;

    [[nodiscard]]
    std::size_t pendingFrames() const noexcept;

    [[nodiscard]]
    std::size_t pendingBytes() const noexcept;

    [[nodiscard]]
    bool empty() const noexcept;

    [[nodiscard]]
    bool full() const noexcept;

    void closeProducer() noexcept;

    [[nodiscard]]
    bool producerClosed() const noexcept;

  private:
    struct Layout;

    Layout* m_layout = nullptr;
    std::size_t m_mappingSize = 0;
};

#endif