#include "Reader.hpp"

#include "EventFd.hpp"
#include "FileStore.hpp"
#include "SharedRingBuffer.hpp"
#include "TelemetryCodec.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string_view>

int Reader::run(SharedRingBuffer &ring, int dataReadyFd, int spaceReadyFd,
                const ReaderConfig &config) const
{
    using namespace telemetry::protocol;

    std::cout << "[reader] pid=" << getpid() << '\n';

    FileStore store{FileStoreConfig{.maxSegmentBytes = config.maxSegmentBytes,
                                    .syncEveryBytes = config.syncEveryBytes,
                                    .measureLatency = config.statsIntervalMs != 0}};

    using Clock = std::chrono::steady_clock;
    const bool monitoring = config.statsIntervalMs != 0;
    const auto interval = std::chrono::milliseconds(config.statsIntervalMs);
    auto previousSample = Clock::now();
    auto nextSample = previousSample + interval;
    std::uint64_t consumedFrames = 0;
    std::uint64_t previousWritten = 0;
    std::uint64_t storageErrors = 0;

    const auto reportMetrics = [&](std::string_view status) {
        if (!monitoring) {
            return;
        }
        const auto now = Clock::now();
        if (status == "running" && now < nextSample) {
            return;
        }
        const auto storage = store.metrics();
        const double seconds =
            std::max(0.001, std::chrono::duration<double>(now - previousSample).count());
        const double rate = static_cast<double>(storage.writtenBytes - previousWritten) / seconds;

        const auto &writeLatency = store.writeLatency();
        const auto &syncLatency = store.syncLatency();
        std::ostringstream report;
        report << "[reader-metrics] consumed_total=" << consumedFrames
               << " written_bytes_total=" << storage.writtenBytes
               << " synced_bytes_total=" << storage.syncedBytes
               << " unsynced_bytes=" << storage.writtenBytes - storage.syncedBytes
               << " written_bytes_per_sec=" << std::fixed << std::setprecision(1) << rate
               << " sync_successes=" << storage.successfulSyncs
               << " storage_errors=" << storageErrors
               << " write_latency_samples=" << writeLatency.count()
               << " write_p50_us=" << writeLatency.percentile(50)
               << " write_p95_us=" << writeLatency.percentile(95)
               << " write_p99_us=" << writeLatency.percentile(99)
               << " sync_latency_samples=" << syncLatency.count()
               << " sync_p50_us=" << syncLatency.percentile(50)
               << " sync_p95_us=" << syncLatency.percentile(95)
               << " sync_p99_us=" << syncLatency.percentile(99) << " status=" << status << '\n';
        std::cout << report.str();
        previousSample = now;
        previousWritten = storage.writtenBytes;
        nextSample = now + interval;
    };

    const auto finish = [&](int result) {
        reportMetrics(result == EXIT_SUCCESS ? "success" : "failure");
        return result;
    };

    if (!store.openFile(config.dataFilePath)) {
        ++storageErrors;
        return finish(EXIT_FAILURE);
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
            ++storageErrors;
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
            consumedFrames += static_cast<std::uint64_t>(popped);
            for (std::size_t index = 0; index < popped; ++index) {
                if (!processFrame({ipcBatch.data() + index * kFrameSize, kFrameSize})) {
                    return finish(EXIT_FAILURE);
                }
            }
            // A continuously busy ring must not prevent periodic sampling.
            reportMetrics("running");
        }

        // Flush even a partially filled batch before sleeping or reporting
        // space available. Otherwise a quiet producer could leave data
        // buffered indefinitely, despite the ring already being empty.
        if (!flushBatch()) {
            return finish(EXIT_FAILURE);
        }

        // Notify the server once after draining the frames currently available.
        if (consumedAny) {
            if (!telemetry::ipc::notifyEvent(spaceReadyFd)) {
                return finish(EXIT_FAILURE);
            }
        }

        // Finish after the producer closes and all queued frames have been consumed.
        if (ring.producerClosed() && ring.empty()) {
            if (!store.sync()) {
                ++storageErrors;
                return finish(EXIT_FAILURE);
            }

            std::cout << "[reader] EOF\n";
            return finish(EXIT_SUCCESS);
        }

        // With monitoring enabled, wait for either IPC data or the next report.
        // The Reader is the only consumer of dataReadyFd, so a readable eventfd
        // can still be consumed using the existing blocking helper.
        reportMetrics("running");
        if (monitoring) {
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(nextSample - Clock::now()).count();
            const int timeout = static_cast<int>(std::clamp<std::int64_t>(
                remaining, 0, static_cast<std::int64_t>(config.statsIntervalMs)));
            pollfd event{dataReadyFd, POLLIN, 0};
            const int ready = poll(&event, 1, timeout);
            if (ready == 0 || (ready == -1 && errno == EINTR)) {
                continue;
            }
            if (ready == -1 || (event.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
                (event.revents & POLLIN) == 0) {
                std::cerr << "[reader] dataReady poll failed\n";
                return finish(EXIT_FAILURE);
            }
        }

        // This wait does not detect producer death.
        if (!telemetry::ipc::consumeEvent(dataReadyFd)) {
            return finish(EXIT_FAILURE);
        }
    }
}
