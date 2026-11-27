#ifndef LATENCY_HISTOGRAM_HPP
#define LATENCY_HISTOGRAM_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

// Cumulative power-of-two microsecond buckets. Percentiles are upper bounds.
class LatencyHistogram
{
  public:
    void record(std::uint64_t microseconds) noexcept
    {
        const auto index = static_cast<std::size_t>(
            std::bit_width(microseconds == 0 ? std::uint64_t{0} : microseconds - 1));
        ++m_buckets[index];
        ++m_count;
    }

    [[nodiscard]] std::uint64_t count() const noexcept
    {
        return m_count;
    }

    // Nearest rank, rounded up to the containing bucket's upper bound.
    // Zero denotes no observations (or an invalid percentile argument).
    [[nodiscard]] std::uint64_t percentile(unsigned percent) const noexcept
    {
        if (m_count == 0 || percent == 0 || percent > 100) {
            return 0;
        }
        // Split the multiplication to avoid overflowing a uint64_t count.
        const auto rank = (m_count / 100) * percent + ((m_count % 100) * percent + 99) / 100;
        std::uint64_t cumulative = 0;
        for (std::size_t index = 0; index < m_buckets.size(); ++index) {
            cumulative += m_buckets[index];
            if (cumulative >= rank) {
                return index == 64 ? std::numeric_limits<std::uint64_t>::max()
                                   : std::uint64_t{1} << index;
            }
        }
        return 0;
    }

  private:
    std::array<std::uint64_t, 65> m_buckets{};
    std::uint64_t m_count = 0;
};

#endif
