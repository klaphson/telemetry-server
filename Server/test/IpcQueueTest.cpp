#include "IpcQueue.hpp"
#include <catch2/catch_test_macros.hpp>

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
        queue.buffer.assign(test.pending, 'x');
        REQUIRE(queue.pendingBytes() == test.pending);
        REQUIRE(queue.shouldPauseIngress() == test.pause);
        REQUIRE(queue.canResumeIngress() == test.resume);
    }
}

TEST_CASE("thresholds count unread bytes across compaction", "[ipc_queue]")
{
    IpcQueue queue;
    queue.buffer.assign(IpcQueue::maxBufferSize, 'x');
    queue.offset = queue.buffer.size() - IpcQueue::pauseIngressThreshold;
    REQUIRE(queue.shouldPauseIngress());
    ++queue.offset;
    REQUIRE(!queue.shouldPauseIngress());
    REQUIRE(!queue.canResumeIngress());

    queue.offset = queue.buffer.size() - IpcQueue::resumeIngressThreshold;
    REQUIRE(queue.canResumeIngress());
    queue.compactIfNeeded();
    REQUIRE(queue.offset == 0);
    REQUIRE(queue.pendingBytes() == IpcQueue::resumeIngressThreshold);
    REQUIRE(!queue.shouldPauseIngress());
    REQUIRE(queue.canResumeIngress());

    queue.offset = queue.buffer.size();
    REQUIRE(queue.empty());
    REQUIRE(!queue.shouldPauseIngress());
    REQUIRE(queue.canResumeIngress());
}

TEST_CASE("record newline counts toward pause threshold", "[ipc_queue]")
{
    IpcQueue queue;
    REQUIRE(queue.appendLine(std::string(IpcQueue::pauseIngressThreshold - 1, 'x')));
    REQUIRE(queue.pendingBytes() == IpcQueue::pauseIngressThreshold);
    REQUIRE(queue.buffer.back() == '\n');
    REQUIRE(queue.shouldPauseIngress());
}

TEST_CASE("hard limit preserves records and reuses consumed capacity", "[ipc_queue]")
{
    IpcQueue queue;
    REQUIRE(queue.appendLine(std::string(IpcQueue::maxBufferSize - 1, 'x')));
    const std::string original = queue.buffer;
    REQUIRE(!queue.appendLine("overflow"));
    REQUIRE(queue.buffer == original);
    REQUIRE(queue.pendingBytes() == IpcQueue::maxBufferSize);

    // A consumed prefix must free capacity even before explicit compaction.
    queue.offset = 7;
    REQUIRE(queue.appendLine("record"));
    REQUIRE(queue.pendingBytes() == IpcQueue::maxBufferSize);
    REQUIRE(queue.buffer.ends_with("\nrecord\n"));
}
