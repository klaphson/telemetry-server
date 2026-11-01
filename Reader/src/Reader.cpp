#include "Reader.hpp"

#include "EventFd.hpp"
#include "FileStore.hpp"
#include "SharedRingBuffer.hpp"
#include "TelemetryCodec.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>

int Reader::run(SharedRingBuffer &ring, int dataReadyFd, int spaceReadyFd,
                const ReaderConfig &config) const
{
    using namespace telemetry::protocol;

    std::cout << "[reader] pid=" << getpid() << '\n';

    FileStore store{FileStoreConfig{.maxSegmentBytes = config.maxSegmentBytes,
                                    .syncEveryBytes = config.syncEveryBytes}};

    if (!store.openFile(config.dataFilePath)) {
        return EXIT_FAILURE;
    }

    std::cout << "[reader] reading telemetry from shared memory\n";

    // 2048 frames per 64 KiB write when the ring is sufficiently busy.
    // Keep the staging buffer in the Reader; FileStore remains responsible
    // for segment rotation and syncing the configured byte intervals.
    constexpr std::size_t writeBatchBytes = 64 * 1024;
    static_assert(writeBatchBytes % kFrameSize == 0);
    std::array<std::byte, writeBatchBytes> batch{};
    std::size_t batchBytes = 0;

    const auto flushBatch = [&]() {
        if (batchBytes == 0) {
            return true;
        }
        if (!store.appendFrames({batch.data(), batchBytes})) {
            return false;
        }
        batchBytes = 0;
        return true;
    };

    // IPC always uses batched transfers. The
    // storage batch remains 64 KiB and keeps all durability boundaries.
    const auto processFrame = [&](std::span<const std::byte> frame) {
        const DecodeResult decoded = decode(frame);

        if (!decoded) {
            // Persist valid records before a malformed IPC frame.
            if (!flushBatch()) {
                return false;
            }
            std::cerr << "[reader] invalid IPC frame: " << toString(decoded.error) << '\n';
            return false;
        }

        std::copy(frame.begin(), frame.end(), batch.data() + batchBytes);
        batchBytes += kFrameSize;
        if (batchBytes == batch.size() && !flushBatch()) {
            return false;
        }

        const TelemetryRecord &record = decoded.record;
        if (!config.quiet) {
            std::cout << "[reader] telemetry:" << " sensor=" << record.sensorId
                      << " metric=" << record.metricId << " timestamp_ns=" << record.timestampNs
                      << " value=" << record.value << '\n';
        }
        return true;
    };

    // Up to 128 frames are popped under a single release store to tail.
    // The 4 KiB IPC batch is intentionally independent of the 64 KiB
    // persistence batch, which is flushed before waiting for new input.
    constexpr std::size_t ipcBatchBytes = 4096;
    static_assert(ipcBatchBytes % kFrameSize == 0);
    std::array<std::byte, ipcBatchBytes> ipcBatch{};

    while (true) {
        bool consumedAny = false;

        while (true) {
            const std::size_t popped = ring.tryPopBatch(ipcBatch);
            if (popped == 0) {
                break;
            }
            consumedAny = true;
            for (std::size_t index = 0; index < popped; ++index) {
                if (!processFrame({ipcBatch.data() + index * kFrameSize, kFrameSize})) {
                    return EXIT_FAILURE;
                }
            }
        }

        // Flush even a partially filled batch before sleeping or reporting
        // space available. Otherwise a quiet producer could leave data
        // buffered indefinitely, despite the ring already being empty.
        if (!flushBatch()) {
            return EXIT_FAILURE;
        }

        // Notify the server once after draining the frames currently available.
        if (consumedAny) {
            if (!telemetry::ipc::notifyEvent(spaceReadyFd)) {
                return EXIT_FAILURE;
            }
        }

        // Finish after the producer closes and all queued frames have been consumed.
        if (ring.producerClosed() && ring.empty()) {
            if (!store.sync()) {
                return EXIT_FAILURE;
            }

            std::cout << "[reader] EOF\n";
            return EXIT_SUCCESS;
        }

        // Wait for a producer notification; this wait does not detect producer death.
        if (!telemetry::ipc::consumeEvent(dataReadyFd)) {
            return EXIT_FAILURE;
        }
    }
}
