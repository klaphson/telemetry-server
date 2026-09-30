#include "FileStore.hpp"
#include "TelemetryCodec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
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

} // namespace