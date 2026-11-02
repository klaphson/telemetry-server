// Educational C++20 producer/consumer laboratory. No production server code.
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t frameBytes = 32;
constexpr std::size_t capacityFrames = 4096;
using Frame = std::array<std::byte, frameBytes>;

Frame frameFor(std::uint64_t sequence)
{
    Frame frame{};
    for (std::size_t i = 0; i < frame.size(); ++i) {
        frame[i] = std::byte{static_cast<unsigned char>(
            ((sequence >> ((i % 8) * 8U)) ^ (i * 29U)) & 0xFFU)};
    }
    return frame;
}

// All queue state is protected by one mutex; the CVs avoid busy waiting.
class MutexChannel {
  public:
    std::size_t push(std::span<const Frame> input, std::stop_token stop)
    {
        if (input.empty()) return 0;
        std::unique_lock lock(m_mutex);
        if (!m_space.wait(lock, stop, [&] { return m_closed || m_head - m_tail < capacityFrames; }) ||
            m_closed) return 0;
        const auto count = std::min(input.size(), capacityFrames - (m_head - m_tail));
        for (std::size_t i = 0; i < count; ++i) m_slots[(m_head + i) % capacityFrames] = input[i];
        m_head += count;
        lock.unlock();
        m_data.notify_one();
        return count;
    }

    std::size_t pop(std::span<Frame> output, std::stop_token stop)
    {
        if (output.empty()) return 0;
        std::unique_lock lock(m_mutex);
        if (!m_data.wait(lock, stop, [&] { return m_closed || m_head != m_tail; })) return 0;
        const auto count = std::min(output.size(), m_head - m_tail);
        // Copy before freeing slots so producer cannot overwrite unread data.
        for (std::size_t i = 0; i < count; ++i)
            output[i] = m_slots[(m_tail + i) % capacityFrames];
        m_tail += count;
        lock.unlock();
        m_space.notify_one();
        return count;
    }

    void close()
    {
        { std::lock_guard lock(m_mutex); m_closed = true; }
        m_data.notify_all();
        m_space.notify_all();
    }

  private:
    std::mutex m_mutex;
    std::condition_variable_any m_data, m_space;
    std::array<Frame, capacityFrames> m_slots{};
    std::size_t m_head = 0, m_tail = 0;
    bool m_closed = false;
};

// SPSC: producer alone writes head, consumer alone writes tail.
// A shared mutex only coordinates sleep/wakeup; not the slot transfer.
class AtomicChannel {
  public:
    std::size_t push(std::span<const Frame> input, std::stop_token stop)
    {
        if (input.empty()) return 0;
        while (!stop.stop_requested()) {
            if (m_closed.load(std::memory_order_acquire)) return 0;
            const auto head = m_head.load(std::memory_order_relaxed);
            const auto tail = m_tail.load(std::memory_order_acquire);
            const auto count = std::min(input.size(), capacityFrames - static_cast<std::size_t>(head - tail));
            if (count != 0) {
                for (std::size_t i = 0; i < count; ++i) m_slots[(head + i) % capacityFrames] = input[i];
                m_head.store(head + count, std::memory_order_release);
                { std::lock_guard lock(m_waitMutex); }
                m_data.notify_one();
                return count;
            }
            std::unique_lock lock(m_waitMutex);
            if (!m_space.wait(lock, stop, [&] {
                return m_closed.load(std::memory_order_acquire) ||
                       m_head.load(std::memory_order_relaxed) -
                           m_tail.load(std::memory_order_acquire) < capacityFrames;
            })) return 0;
        }
        return 0;
    }

    std::size_t pop(std::span<Frame> output, std::stop_token stop)
    {
        if (output.empty()) return 0;
        while (!stop.stop_requested()) {
            const auto tail = m_tail.load(std::memory_order_relaxed);
            const auto head = m_head.load(std::memory_order_acquire);
            const auto count = std::min(output.size(), static_cast<std::size_t>(head - tail));
            if (count != 0) {
                for (std::size_t i = 0; i < count; ++i) output[i] = m_slots[(tail + i) % capacityFrames];
                m_tail.store(tail + count, std::memory_order_release);
                { std::lock_guard lock(m_waitMutex); }
                m_space.notify_one();
                return count;
            }
            if (m_closed.load(std::memory_order_acquire)) return 0;
            std::unique_lock lock(m_waitMutex);
            if (!m_data.wait(lock, stop, [&] {
                return m_closed.load(std::memory_order_acquire) ||
                       m_head.load(std::memory_order_acquire) != m_tail.load(std::memory_order_relaxed);
            })) return 0;
        }
        return 0;
    }

    void close()
    {
        m_closed.store(true, std::memory_order_release);
        { std::lock_guard lock(m_waitMutex); }
        m_data.notify_all();
        m_space.notify_all();
    }

  private:
    alignas(64) std::atomic<std::uint64_t> m_head{0};
    alignas(64) std::atomic<std::uint64_t> m_tail{0};
    std::atomic<bool> m_closed{false};
    std::array<Frame, capacityFrames> m_slots{};
    std::mutex m_waitMutex;
    std::condition_variable_any m_data, m_space;
};

struct Result { double seconds; std::uint64_t frames; };

template <class Channel>
Result run(std::uint64_t frames, std::size_t batch)
{
    Channel channel;
    std::uint64_t consumed = 0, produced = 0;
    std::exception_ptr producerError, consumerError;
    const auto start = std::chrono::steady_clock::now();
    std::jthread reader([&](std::stop_token stop) {
        try {
            std::array<Frame, 1024> output{};
            while (true) {
                const auto count = channel.pop(std::span{output}.first(batch), stop);
                if (count == 0) break;
                for (std::size_t i = 0; i < count; ++i) {
                    if (output[i] != frameFor(consumed + 1))
                        throw std::runtime_error("frame corruption or ordering error");
                    ++consumed;
                }
            }
        } catch (...) { consumerError = std::current_exception(); channel.close(); }
    });
    std::jthread server([&](std::stop_token stop) {
        try {
            std::array<Frame, 1024> input{};
            while (produced < frames) {
                const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(batch, frames - produced));
                for (std::size_t i = 0; i < want; ++i) input[i] = frameFor(produced + i + 1);
                const auto count = channel.push(std::span{input}.first(want), stop);
                if (count == 0) throw std::runtime_error("producer stopped before completion");
                produced += count;
            }
        } catch (...) { producerError = std::current_exception(); }
        channel.close();
    });
    server.join();
    reader.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (producerError) std::rethrow_exception(producerError);
    if (consumerError) std::rethrow_exception(consumerError);
    if (produced != frames || consumed != frames) throw std::runtime_error("incomplete transfer");
    return {seconds, consumed};
}

unsigned number(std::string_view s, unsigned lo, unsigned hi, std::string_view flag)
{
    unsigned x = 0;
    auto [pos, ec] = std::from_chars(s.data(), s.data() + s.size(), x);
    if (ec != std::errc{} || pos != s.data() + s.size() || x < lo || x > hi)
        throw std::runtime_error("invalid " + std::string(flag));
    return x;
}

void selfTest()
{
    for (auto frames : {1U, 127U, 4096U, 8193U, 25000U}) {
        for (auto batch : {1U, 128U}) {
            run<MutexChannel>(frames, batch);
            run<AtomicChannel>(frames, batch);
        }
    }
    // Closing must reject new frames but still deliver everything queued.
    for (bool atomic : {false, true}) {
        const Frame expected = frameFor(17);
        if (atomic) {
            AtomicChannel channel;
            if (channel.push(std::span{&expected, 1}, {}) != 1)
                throw std::runtime_error("atomic close setup failed");
            channel.close();
            Frame output{};
            if (channel.pop(std::span{&output, 1}, {}) != 1 || output != expected ||
                channel.pop(std::span{&output, 1}, {}) != 0 ||
                channel.push(std::span{&expected, 1}, {}) != 0)
                throw std::runtime_error("atomic close semantics failed");
        } else {
            MutexChannel channel;
            if (channel.push(std::span{&expected, 1}, {}) != 1)
                throw std::runtime_error("mutex close setup failed");
            channel.close();
            Frame output{};
            if (channel.pop(std::span{&output, 1}, {}) != 1 || output != expected ||
                channel.pop(std::span{&output, 1}, {}) != 0 ||
                channel.push(std::span{&expected, 1}, {}) != 0)
                throw std::runtime_error("mutex close semantics failed");
        }
    }
    for (bool atomic : {false, true}) {
        std::atomic<bool> wasStopped{false};
        if (atomic) {
            AtomicChannel channel;
            std::jthread waiter([&](std::stop_token stop) {
                Frame f{};
                wasStopped.store(channel.pop(std::span{&f, 1}, stop) == 0);
            });
            waiter.request_stop(); waiter.join();
        } else {
            MutexChannel channel;
            std::jthread waiter([&](std::stop_token stop) {
                Frame f{};
                wasStopped.store(channel.pop(std::span{&f, 1}, stop) == 0);
            });
            waiter.request_stop(); waiter.join();
        }
        if (!wasStopped.load()) throw std::runtime_error("stop_token did not wake consumer");
    }
    std::cout << "thread sync lab self-test: PASS\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        unsigned frames = 1'000'000, batch = 128, repeat = 5;
        std::string mode = "both";
        bool selftest = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--help") {
                std::cout << "Usage: telemetry-thread-lab [--frames 1..10000000] [--batch 1..1024] "
                             "[--repeat 1..20] [--mode mutex|atomic|both] [--self-test]\n";
                return 0;
            }
            if (arg == "--self-test") { selftest = true; continue; }
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + std::string(arg));
            const std::string_view value = argv[++i];
            if (arg == "--frames") frames = number(value, 1, 10'000'000, arg);
            else if (arg == "--batch") batch = number(value, 1, 1024, arg);
            else if (arg == "--repeat") repeat = number(value, 1, 20, arg);
            else if (arg == "--mode" && (value == "mutex" || value == "atomic" || value == "both"))
                mode = value;
            else throw std::runtime_error("invalid option " + std::string(arg));
        }
        if (selftest) { selfTest(); return 0; }
        for (const auto& selected : {std::string("mutex"), std::string("atomic")}) {
            if (mode != "both" && mode != selected) continue;
            std::vector<double> rates;
            for (unsigned i = 0; i < repeat; ++i) {
                const auto result = selected == "mutex" ? run<MutexChannel>(frames, batch)
                                                        : run<AtomicChannel>(frames, batch);
                const auto rate = static_cast<double>(result.frames) / result.seconds;
                rates.push_back(rate);
                std::cout << "mode=" << selected << " run=" << (i + 1) << " frames=" << result.frames
                          << " seconds=" << result.seconds << " frames_per_second=" << rate
                          << " verified=yes\n";
            }
            std::sort(rates.begin(), rates.end());
            const auto middle = rates.size() / 2;
            const double median = (rates.size() % 2 == 0)
                                      ? (rates[middle - 1] + rates[middle]) / 2.0
                                      : rates[middle];
            std::cout << "SUMMARY mode=" << selected << " median_frames_per_second="
                      << median << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "thread sync lab failed: " << e.what() << '\n';
        return 1;
    }
}
