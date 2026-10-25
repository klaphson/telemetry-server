#include "Reader.hpp"

#include "EventFd.hpp"
#include "FileStore.hpp"
#include "SharedRingBuffer.hpp"
#include "TelemetryCodec.hpp"

#include <unistd.h>

#include <cstdlib>
#include <iostream>

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

    Frame frame{};

    while (true) {
        bool consumedAny = false;

        // Try to receive every currently available frame.
        while (ring.tryPop(frame)) {
            consumedAny = true;

            const DecodeResult decoded = decode(frame);

            if (!decoded) {
                std::cerr << "[reader] invalid IPC frame: " << toString(decoded.error) << '\n';
                return EXIT_FAILURE;
            }

            if (!store.appendFrame(frame)) {
                return EXIT_FAILURE;
            }

            const TelemetryRecord &record = decoded.record;

            if (!config.quiet) {
                std::cout << "[reader] telemetry:" << " sensor=" << record.sensorId
                          << " metric=" << record.metricId << " timestamp_ns=" << record.timestampNs
                          << " value=" << record.value << '\n';
            }
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
