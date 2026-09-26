#ifndef BUFFER_HPP
#define BUFFER_HPP

#include <cstddef>
#include <vector>

struct Buffer {
    std::vector<std::byte> data;
    std::size_t offset = 0;

    [[nodiscard]]
    bool empty() const noexcept
    {
        return offset >= data.size();
    }

    [[nodiscard]]
    std::size_t pendingBytes() const noexcept
    {
        return empty() ? 0 : data.size() - offset;
    }

    void compact(std::size_t threshold = 4096)
    {
        if (offset == 0) {
            return;
        }

        if (empty()) {
            data.clear();
            offset = 0;
            return;
        }

        if (offset >= threshold) {
            using Difference = std::vector<std::byte>::difference_type;

            data.erase(data.begin(), data.begin() + static_cast<Difference>(offset));

            offset = 0;
        }
    }
};

#endif // BUFFER_HPP
