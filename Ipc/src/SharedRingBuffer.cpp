#include "SharedRingBuffer.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{

constexpr std::size_t cacheLineSize = 64;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(telemetry::protocol::Frame) == telemetry::protocol::kFrameSize);

} // namespace

struct SharedRingBuffer::Layout {
    alignas(cacheLineSize) std::atomic<std::uint64_t> head{0};

    alignas(cacheLineSize) std::atomic<std::uint64_t> tail{0};

    alignas(cacheLineSize) std::atomic<std::uint32_t> producerClosed{0};

    alignas(cacheLineSize) std::array<telemetry::protocol::Frame, capacityFrames> slots{};
};

SharedRingBuffer::~SharedRingBuffer()
{
    if (m_layout == nullptr) {
        return;
    }

    std::destroy_at(m_layout);

    if (munmap(m_layout, m_mappingSize) == -1) {

        perror("munmap shared ring");
    }
}

bool SharedRingBuffer::create()
{
    if (m_layout != nullptr) {
        return false;
    }

    void* mapping =
        mmap(nullptr, sizeof(Layout), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (mapping == MAP_FAILED) {
        perror("mmap shared ring");
        return false;
    }

    m_layout = std::construct_at(static_cast<Layout*>(mapping));

    m_mappingSize = sizeof(Layout);

    return true;
}

bool SharedRingBuffer::tryPush(std::span<const std::byte> frame) noexcept
{
    using telemetry::protocol::kFrameSize;

    if (m_layout == nullptr || frame.size() != kFrameSize) {
        return false;
    }

    if (producerClosed()) {
        return false;
    }

    const std::uint64_t head = m_layout->head.load(std::memory_order_relaxed);

    const std::uint64_t tail = m_layout->tail.load(std::memory_order_acquire);

    if (head - tail >= capacityFrames) {
        return false;
    }

    auto &slot = m_layout->slots[static_cast<std::size_t>(head % capacityFrames)];

    std::copy(frame.begin(), frame.end(), slot.begin());

    m_layout->head.store(head + 1, std::memory_order_release);

    return true;
}

bool SharedRingBuffer::tryPop(std::span<std::byte> frame) noexcept
{
    using telemetry::protocol::kFrameSize;

    if (m_layout == nullptr || frame.size() != kFrameSize) {
        return false;
    }

    const std::uint64_t tail = m_layout->tail.load(std::memory_order_relaxed);

    const std::uint64_t head = m_layout->head.load(std::memory_order_acquire);

    if (tail == head) {
        return false;
    }

    const auto &slot = m_layout->slots[static_cast<std::size_t>(tail % capacityFrames)];

    std::copy(slot.begin(), slot.end(), frame.begin());

    m_layout->tail.store(tail + 1, std::memory_order_release);

    return true;
}


std::size_t SharedRingBuffer::tryPushBatch(std::span<const std::byte> frames) noexcept
{
    using telemetry::protocol::kFrameSize;

    if (m_layout == nullptr || frames.empty() || frames.size() % kFrameSize != 0 ||
        producerClosed()) {
        return 0;
    }

    const std::uint64_t head = m_layout->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = m_layout->tail.load(std::memory_order_acquire);
    const auto freeFrames = capacityFrames - static_cast<std::size_t>(head - tail);
    const auto count = std::min(frames.size() / kFrameSize, freeFrames);

    if (count == 0) {
        return 0;
    }

    const auto index = static_cast<std::size_t>(head % capacityFrames);
    const auto first = std::min(count, capacityFrames - index);
    std::memcpy(&m_layout->slots[index], frames.data(), first * kFrameSize);

    if (first != count) {
        std::memcpy(&m_layout->slots[0], frames.data() + first * kFrameSize,
                    (count - first) * kFrameSize);
    }

    // A single release store publishes the entire copied batch. The consumer
    // sees fully initialized frames after its acquire load of head.
    m_layout->head.store(head + count, std::memory_order_release);
    return count;
}

std::size_t SharedRingBuffer::tryPopBatch(std::span<std::byte> frames) noexcept
{
    using telemetry::protocol::kFrameSize;

    if (m_layout == nullptr || frames.empty() || frames.size() % kFrameSize != 0) {
        return 0;
    }

    const std::uint64_t tail = m_layout->tail.load(std::memory_order_relaxed);
    const std::uint64_t head = m_layout->head.load(std::memory_order_acquire);
    const auto count =
        std::min(frames.size() / kFrameSize, static_cast<std::size_t>(head - tail));

    if (count == 0) {
        return 0;
    }

    const auto index = static_cast<std::size_t>(tail % capacityFrames);
    const auto first = std::min(count, capacityFrames - index);
    std::memcpy(frames.data(), &m_layout->slots[index], first * kFrameSize);

    if (first != count) {
        std::memcpy(frames.data() + first * kFrameSize, &m_layout->slots[0],
                    (count - first) * kFrameSize);
    }

    // Release the occupied slots only AFTER copying to private storage.
    m_layout->tail.store(tail + count, std::memory_order_release);
    return count;
}

std::size_t SharedRingBuffer::pendingFrames() const noexcept
{
    if (m_layout == nullptr) {
        return 0;
    }

    /*
     * Load tail first.
     *
     * tail only increases and head never moves
     * backwards. Loading in this order prevents
     * observing a newer tail with an older head,
     * which could make unsigned subtraction wrap.
     */
    const std::uint64_t tail = m_layout->tail.load(std::memory_order_acquire);

    const std::uint64_t head = m_layout->head.load(std::memory_order_acquire);

    return static_cast<std::size_t>(head - tail);
}

std::size_t SharedRingBuffer::pendingBytes() const noexcept
{
    return pendingFrames() * telemetry::protocol::kFrameSize;
}

bool SharedRingBuffer::empty() const noexcept
{
    return pendingFrames() == 0;
}

bool SharedRingBuffer::full() const noexcept
{
    return pendingFrames() >= capacityFrames;
}

void SharedRingBuffer::closeProducer() noexcept
{
    if (m_layout == nullptr) {
        return;
    }

    m_layout->producerClosed.store(1, std::memory_order_release);
}

bool SharedRingBuffer::producerClosed() const noexcept
{
    if (m_layout == nullptr) {
        return true;
    }

    return m_layout->producerClosed.load(std::memory_order_acquire) != 0;
}