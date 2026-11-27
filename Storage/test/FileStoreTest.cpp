#include "FileStore.hpp"
#include "LatencyHistogram.hpp"
#include "TelemetryCodec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{

class TemporaryPath
{
  public:
    TemporaryPath()
    {
        std::array<char, 64> path{"/tmp/telemetry-store-XXXXXX"};

        const int fd = mkstemp(path.data());

        REQUIRE(fd >= 0);

        close(fd);

        m_path = path.data();

        REQUIRE(unlink(m_path.c_str()) == 0);
    }

    ~TemporaryPath()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    const std::string &get() const
    {
        return m_path;
    }

  private:
    std::string m_path;
};

class TemporaryDirectory
{
  public:
    TemporaryDirectory()
    {
        std::array<char, 64> path{"/tmp/telemetry-dir-XXXXXX"};

        char* result = mkdtemp(path.data());

        REQUIRE(result != nullptr);

        m_path = result;
    }

    ~TemporaryDirectory()
    {
        std::error_code error;

        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]]
    std::filesystem::path file(std::string_view name) const
    {
        return std::filesystem::path{m_path} / name;
    }

    [[nodiscard]]
    const std::string &path() const
    {
        return m_path;
    }

  private:
    std::string m_path;
};

TEST_CASE("LatencyHistogram estimates nearest-rank percentiles from bounded buckets")
{
    LatencyHistogram histogram;
    REQUIRE(histogram.count() == 0);
    REQUIRE(histogram.percentile(99) == 0);

    for (int i = 0; i < 95; ++i) {
        histogram.record(3);
    }
    for (int i = 0; i < 4; ++i) {
        histogram.record(9);
    }
    histogram.record(1000);

    REQUIRE(histogram.count() == 100);
    REQUIRE(histogram.percentile(50) == 4);
    REQUIRE(histogram.percentile(95) == 4);
    REQUIRE(histogram.percentile(99) == 16);
    REQUIRE(histogram.percentile(100) == 1024);
    REQUIRE(histogram.percentile(0) == 0);
    REQUIRE(histogram.percentile(101) == 0);
}

TEST_CASE("LatencyHistogram handles zero, exact boundaries and the largest sample")
{
    LatencyHistogram histogram;
    histogram.record(0);
    histogram.record(1);
    histogram.record(2);
    histogram.record(3);
    histogram.record(std::numeric_limits<std::uint64_t>::max());

    REQUIRE(histogram.percentile(20) == 1);
    REQUIRE(histogram.percentile(40) == 1);
    REQUIRE(histogram.percentile(60) == 2);
    REQUIRE(histogram.percentile(80) == 4);
    REQUIRE(histogram.percentile(100) == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("FileStore latency collection is opt-in and spans rotation")
{
    using namespace telemetry::protocol;
    TemporaryDirectory directory;
    const auto frame = encode(TelemetryRecord{1, 2, 3, 4.0});

    FileStore disabled;
    REQUIRE(disabled.openFile(directory.file("disabled.bin").string()));
    REQUIRE(disabled.appendFrame(frame));
    REQUIRE(disabled.sync());
    REQUIRE(disabled.writeLatency().count() == 0);
    REQUIRE(disabled.syncLatency().count() == 0);

    FileStore enabled{FileStoreConfig{
        .maxSegmentBytes = 2 * kFrameSize, .syncEveryBytes = 0, .measureLatency = true}};
    REQUIRE(enabled.openFile(directory.file("enabled.bin").string()));
    REQUIRE(enabled.writeLatency().count() == 0);
    REQUIRE(enabled.syncLatency().count() == 0);
    REQUIRE(enabled.appendFrame(frame));
    REQUIRE(enabled.appendFrame(frame));
    REQUIRE(enabled.appendFrame(frame)); // Sync during rotation.
    REQUIRE(enabled.writeLatency().count() >= 3);
    REQUIRE(enabled.syncLatency().count() == 1);
    REQUIRE(enabled.sync());
    REQUIRE(enabled.syncLatency().count() == 2);
    REQUIRE(enabled.syncLatency().count() == enabled.metrics().successfulSyncs);
    for (const auto* histogram : {&enabled.writeLatency(), &enabled.syncLatency()}) {
        REQUIRE(histogram->percentile(50) >= 1);
        REQUIRE(histogram->percentile(50) <= histogram->percentile(95));
        REQUIRE(histogram->percentile(95) <= histogram->percentile(99));
    }
}

TEST_CASE("FileStore appends binary telemetry frames")
{
    using namespace telemetry::protocol;

    TemporaryPath path;

    const TelemetryRecord first{1, 2, 3, 1.0};

    const TelemetryRecord second{4, 5, 6, 2.0};

    const Frame firstFrame = encode(first);

    const Frame secondFrame = encode(second);

    {
        FileStore store;

        REQUIRE(store.openFile(path.get()));

        REQUIRE(store.appendFrame(firstFrame));

        REQUIRE(store.appendFrame(secondFrame));

        REQUIRE(store.sync());
    }

    REQUIRE(std::filesystem::file_size(path.get()) == 2 * kFrameSize);

    std::ifstream file(path.get(), std::ios::binary);

    std::vector<std::byte> bytes(2 * kFrameSize);

    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    REQUIRE(file.good());

    REQUIRE(std::equal(firstFrame.begin(), firstFrame.end(), bytes.begin()));

    REQUIRE(std::equal(secondFrame.begin(), secondFrame.end(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(kFrameSize)));
}

TEST_CASE("FileStore metrics distinguish writes from sync across rotation")
{
    using namespace telemetry::protocol;
    TemporaryDirectory directory;
    const auto path = directory.file("metrics.bin");
    FileStore store{FileStoreConfig{.maxSegmentBytes = 2 * kFrameSize, .syncEveryBytes = 0}};
    const Frame frame = encode(TelemetryRecord{1, 2, 3, 4.0});

    REQUIRE(store.openFile(path.string()));
    REQUIRE(store.metrics().writtenBytes == 0);
    REQUIRE(store.appendFrame(frame));
    REQUIRE(store.appendFrame(frame));
    REQUIRE(store.metrics().writtenBytes == 2 * kFrameSize);
    REQUIRE(store.metrics().syncedBytes == 0);
    REQUIRE(store.metrics().successfulSyncs == 0);

    REQUIRE(store.appendFrame(frame)); // Rotates and syncs the previous segment.
    REQUIRE(store.metrics().writtenBytes == 3 * kFrameSize);
    REQUIRE(store.metrics().syncedBytes == 2 * kFrameSize);
    REQUIRE(store.metrics().successfulSyncs == 1);

    REQUIRE(store.sync());
    REQUIRE(store.metrics().syncedBytes == 3 * kFrameSize);
    REQUIRE(store.metrics().successfulSyncs == 2);
}

TEST_CASE("FileStore removes partial frame tail on open")
{
    using namespace telemetry::protocol;

    TemporaryPath path;

    const Frame frame = encode(TelemetryRecord{1, 2, 3, 4.0});

    {
        const int fd = open(path.get().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);

        REQUIRE(fd >= 0);

        REQUIRE(write(fd, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()));

        const std::array<std::byte, 7> garbage{};

        REQUIRE(write(fd, garbage.data(), garbage.size()) == static_cast<ssize_t>(garbage.size()));

        close(fd);
    }

    REQUIRE(std::filesystem::file_size(path.get()) == kFrameSize + 7);

    {
        FileStore store;

        REQUIRE(store.openFile(path.get()));
    }

    REQUIRE(std::filesystem::file_size(path.get()) == kFrameSize);
}

TEST_CASE("FileStore rotates before a frame would exceed segment size")
{
    using namespace telemetry::protocol;

    TemporaryDirectory directory;

    const auto activePath = directory.file("telemetry.bin");

    FileStoreConfig config{.maxSegmentBytes = 2 * kFrameSize, .syncEveryBytes = kFrameSize};

    const Frame first = encode(TelemetryRecord{1, 1, 1, 1.0});

    const Frame second = encode(TelemetryRecord{2, 2, 2, 2.0});

    const Frame third = encode(TelemetryRecord{3, 3, 3, 3.0});

    {
        FileStore store{config};

        REQUIRE(store.openFile(activePath.string()));

        REQUIRE(store.appendFrame(first));

        REQUIRE(store.appendFrame(second));

        REQUIRE(store.appendFrame(third));

        REQUIRE(store.sync());
    }

    REQUIRE(std::filesystem::file_size(activePath) == kFrameSize);

    std::size_t segmentCount = 0;

    for (const auto &entry : std::filesystem::directory_iterator{directory.path()}) {

        const std::string name = entry.path().filename().string();

        if (name.starts_with("telemetry.bin.") && name.ends_with(".segment")) {

            ++segmentCount;

            REQUIRE(entry.file_size() == 2 * kFrameSize);
        }
    }

    REQUIRE(segmentCount == 1);
}

TEST_CASE("FileStore appends a batch of complete frames in order")
{
    using namespace telemetry::protocol;

    TemporaryPath path;
    const Frame first = encode(TelemetryRecord{1, 2, 11, 1.0});
    const Frame second = encode(TelemetryRecord{1, 2, 12, 2.0});
    const Frame third = encode(TelemetryRecord{1, 2, 13, 3.0});

    std::vector<std::byte> batch;
    for (const auto &frame : {first, second, third}) {
        batch.insert(batch.end(), frame.begin(), frame.end());
    }

    {
        FileStore store;
        REQUIRE(store.openFile(path.get()));
        REQUIRE(store.appendFrames(batch));
        REQUIRE(store.sync());
    }

    REQUIRE(std::filesystem::file_size(path.get()) == batch.size());
    std::ifstream file(path.get(), std::ios::binary);
    const std::string persisted{std::istreambuf_iterator<char>{file},
                                std::istreambuf_iterator<char>{}};
    const std::string expected{reinterpret_cast<const char*>(batch.data()), batch.size()};
    REQUIRE(persisted == expected);
}

TEST_CASE("FileStore rejects incomplete and empty frame batches")
{
    using namespace telemetry::protocol;

    TemporaryPath path;
    const Frame valid = encode(TelemetryRecord{1, 2, 13, 1.0});
    FileStore store;
    REQUIRE_FALSE(store.appendFrames(valid));
    REQUIRE(store.openFile(path.get()));

    const std::array<std::byte, 31> incomplete{};
    REQUIRE_FALSE(store.appendFrames({}));
    REQUIRE_FALSE(store.appendFrames(incomplete));
    REQUIRE(std::filesystem::file_size(path.get()) == 0);

    REQUIRE(store.appendFrames(valid));
    REQUIRE(std::filesystem::file_size(path.get()) == kFrameSize);
}

TEST_CASE("FileStore splits a batch across segment rotations without losing frames")
{
    using namespace telemetry::protocol;

    TemporaryDirectory directory;
    const auto activePath = directory.file("telemetry.bin");
    const FileStoreConfig config{.maxSegmentBytes = 2 * kFrameSize, .syncEveryBytes = kFrameSize};

    std::vector<std::byte> batch;
    for (std::uint64_t timestamp = 1; timestamp <= 5; ++timestamp) {
        const Frame frame = encode(TelemetryRecord{1, 2, timestamp, 1.0});
        batch.insert(batch.end(), frame.begin(), frame.end());
    }

    {
        FileStore store{config};
        REQUIRE(store.openFile(activePath.string()));
        REQUIRE(store.appendFrames(batch));
        REQUIRE(store.sync());
    }

    std::vector<std::filesystem::path> archives;
    for (const auto &entry : std::filesystem::directory_iterator{directory.path()}) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("telemetry.bin.") && name.ends_with(".segment")) {
            archives.push_back(entry.path());
        }
    }
    std::sort(archives.begin(), archives.end());
    REQUIRE(archives.size() == 2);

    const auto readBytes = [](const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    };
    const std::string expected{reinterpret_cast<const char*>(batch.data()), batch.size()};
    REQUIRE(readBytes(archives[0]) == expected.substr(0, 2 * kFrameSize));
    REQUIRE(readBytes(archives[1]) == expected.substr(2 * kFrameSize, 2 * kFrameSize));
    REQUIRE(readBytes(activePath) == expected.substr(4 * kFrameSize));
}

} // namespace
