#include "IpcQueue.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string_view>

TEST_CASE("pause and resume threshold boundaries", "[ipc_queue]")
{
    struct Case {
        std::size_t pending;
        bool pause;
        bool resume;
    };
    const Case cases[] = {
        {0, false, true},
        {IpcQueue::resumeIngressThreshold - 1, false, true},
        {IpcQueue::resumeIngressThreshold, false, true},
        {IpcQueue::resumeIngressThreshold + 1, false, false},
        {IpcQueue::pauseIngressThreshold - 1, false, false},
        {IpcQueue::pauseIngressThreshold, true, false},
        {IpcQueue::pauseIngressThreshold + 1, true, false},
        {IpcQueue::maxBufferSize, true, false},
    };
    for (const auto &test : cases) {
        CAPTURE(test.pending);
        IpcQueue queue;
        queue.data.assign(test.pending, std::byte{'x'});
        REQUIRE(queue.pendingBytes() == test.pending);
        REQUIRE(queue.shouldPauseIngress() == test.pause);
        REQUIRE(queue.canResumeIngress() == test.resume);
    }
}

TEST_CASE("thresholds count unread bytes across compaction", "[ipc_queue]")
{
    IpcQueue queue;
    queue.data.assign(IpcQueue::maxBufferSize, std::byte{'x'});
    queue.offset = queue.data.size() - IpcQueue::pauseIngressThreshold;
    REQUIRE(queue.shouldPauseIngress());
    ++queue.offset;
    REQUIRE(!queue.shouldPauseIngress());
    REQUIRE(!queue.canResumeIngress());

    queue.offset = queue.data.size() - IpcQueue::resumeIngressThreshold;
    REQUIRE(queue.canResumeIngress());
    queue.compactIfNeeded();
    REQUIRE(queue.offset == 0);
    REQUIRE(queue.pendingBytes() == IpcQueue::resumeIngressThreshold);
    REQUIRE(!queue.shouldPauseIngress());
    REQUIRE(queue.canResumeIngress());

    queue.offset = queue.data.size();
    REQUIRE(queue.empty());
    REQUIRE(!queue.shouldPauseIngress());
    REQUIRE(queue.canResumeIngress());
}

TEST_CASE("frame bytes count toward pause threshold", "[ipc_queue]")
{
    IpcQueue queue;
    const std::vector frame(IpcQueue::pauseIngressThreshold, std::byte{'x'});
    REQUIRE(queue.appendFrame(frame));
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold);
    REQUIRE(queue.data == frame);
    REQUIRE(queue.shouldPauseIngress());
}

TEST_CASE("hard limit preserves frames and reuses consumed capacity", "[ipc_queue]")
{
    IpcQueue queue;
    const std::vector frame(IpcQueue::maxBufferSize, std::byte{'x'});
    REQUIRE(queue.appendFrame(frame));
    const auto original = queue.data;
    const std::string_view overflow = "overflow";
    REQUIRE(!queue.appendFrame(std::as_bytes(std::span(overflow))));
    REQUIRE(queue.data == original);
    REQUIRE(queue.pendingBytes() == IpcQueue::maxBufferSize);

    // A consumed prefix must free capacity even before explicit compaction.
    queue.offset = 7;
    const std::string_view record = "record\n";
    REQUIRE(queue.appendFrame(std::as_bytes(std::span(record))));
    REQUIRE(queue.pendingBytes() == IpcQueue::maxBufferSize);
    const std::string_view contents(reinterpret_cast<const char*>(queue.data.data()),
                                    queue.data.size());
    REQUIRE(contents.ends_with(record));
}

TEST_CASE("shared buffer compaction preserves unread bytes", "[buffer]")
{
    const auto check = [](std::size_t threshold) {
        Buffer buffer;
        buffer.data.assign(threshold, std::byte{'x'});
        buffer.data.push_back(std::byte{0});
        buffer.data.push_back(std::byte{0xff});
        const auto original = buffer.data;

        buffer.compact(threshold);
        REQUIRE(buffer.data == original);

        buffer.offset = threshold - 1;
        buffer.compact(threshold);
        REQUIRE(buffer.data == original);
        REQUIRE(buffer.pendingBytes() == 3);

        buffer.offset = threshold;
        buffer.compact(threshold);
        REQUIRE(buffer.offset == 0);
        REQUIRE(buffer.data == std::vector<std::byte>{std::byte{0}, std::byte{0xff}});

        buffer.offset = buffer.data.size();
        buffer.compact(threshold);
        REQUIRE(buffer.data.empty());
        REQUIRE(buffer.offset == 0);
        REQUIRE(buffer.pendingBytes() == 0);
    };
    check(4096);
    check(IpcQueue::compactionThreshold);
}
