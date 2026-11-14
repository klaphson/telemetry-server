// Educational Linux scheduler laboratory. Does not run the production server.
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    unsigned durationMs = 1000;
    unsigned repeats = 3;
    unsigned niceBOffset = 0;
    std::string mode = "all";
    bool competitor = false;
    bool selfTest = false;
    std::string csv;
};

unsigned number(std::string_view input, unsigned minimum, unsigned maximum, std::string_view name)
{
    unsigned value = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size() || value < minimum ||
        value > maximum) {
        throw std::runtime_error("invalid " + std::string(name));
    }
    return value;
}

Options parseOptions(int argc, char* argv[])
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key{argv[i]};
        if (key == "--help" || key == "-h") {
            std::cout << "Usage: telemetry-scheduler-lab [--mode all|free|same|split] "
                         "[--duration-ms 100..30000] [--repeats 1..20] "
                         "[--nice-b-offset 0..19] [--competitor] [--csv PATH] [--self-test]\n";
            std::exit(0);
        }
        if (key == "--self-test") {
            options.selfTest = true;
            continue;
        }
        if (key == "--competitor") {
            options.competitor = true;
            continue;
        }
        if (i + 1 == argc) {
            throw std::runtime_error("missing value after " + std::string(key));
        }
        const std::string value = argv[++i];
        if (key == "--mode") {
            if (value != "all" && value != "free" && value != "same" && value != "split") {
                throw std::runtime_error("invalid --mode (all|free|same|split)");
            }
            options.mode = value;
        } else if (key == "--duration-ms") {
            options.durationMs = number(value, 100, 30000, key);
        } else if (key == "--repeats") {
            options.repeats = number(value, 1, 20, key);
        } else if (key == "--nice-b-offset") {
            options.niceBOffset = number(value, 0, 19, key);
        } else if (key == "--csv") {
            if (value.empty()) {
                throw std::runtime_error("--csv must not be empty");
            }
            options.csv = value;
        } else {
            throw std::runtime_error("unknown option " + std::string(key));
        }
    }
    return options;
}

std::vector<int> allowedCpus()
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        throw std::runtime_error("sched_getaffinity failed: " + std::string(std::strerror(errno)));
    }
    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &mask)) {
            cpus.push_back(cpu);
        }
    }
    if (cpus.empty()) {
        throw std::runtime_error("no allowed CPUs in affinity mask");
    }
    return cpus;
}

void pinCurrentThread(int cpu)
{
    if (cpu < 0) {
        return;
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    const int error = pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
    if (error != 0) {
        throw std::runtime_error("pthread_setaffinity_np: " + std::string(std::strerror(error)));
    }
}

int increaseCurrentNice(unsigned offset)
{
    errno = 0;
    const int initial = getpriority(PRIO_PROCESS, 0); // Linux: nice is per thread.
    if (initial == -1 && errno != 0) {
        throw std::runtime_error("getpriority failed: " + std::string(std::strerror(errno)));
    }
    if (offset == 0) {
        return initial;
    }
    const int desired = std::min(19, initial + static_cast<int>(offset));
    if (setpriority(PRIO_PROCESS, 0, desired) != 0) {
        throw std::runtime_error("setpriority failed: " + std::string(std::strerror(errno)));
    }
    return desired;
}

double threadCpuSeconds(const rusage &value)
{
    return static_cast<double>(value.ru_utime.tv_sec + value.ru_stime.tv_sec) +
           static_cast<double>(value.ru_utime.tv_usec + value.ru_stime.tv_usec) / 1'000'000.0;
}

double percentile(std::vector<double> &data, double fraction)
{
    if (data.empty()) {
        return 0.0;
    }
    std::sort(data.begin(), data.end());
    const auto index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(data.size())) - 1.0);
    return data[std::min(index, data.size() - 1)];
}

struct WorkerResult {
    std::string role;
    std::string error;
    int requestedCpu = -1;
    int startCpu = -1;
    int lastCpu = -1;
    int nice = 0;
    std::uint64_t chunks = 0;
    std::uint64_t checksum = 0;
    std::uint64_t sampledMigrations = 0;
    long voluntarySwitches = 0;
    long involuntarySwitches = 0;
    double wallSeconds = 0;
    double cpuSeconds = 0;
    double gapP50Us = 0;
    double gapP95Us = 0;
    double gapP99Us = 0;
};

void doWork(WorkerResult &result, int cpu, unsigned niceOffset, std::barrier<> &startGate,
            const Clock::time_point &deadline)
{
    result.requestedCpu = cpu;
    try {
        pinCurrentThread(cpu);
        result.nice = increaseCurrentNice(niceOffset);
    } catch (const std::exception &error) {
        result.error = error.what();
    }

    // Even on setup failure, arrive at the barrier so other threads cannot deadlock.
    startGate.arrive_and_wait();
    if (!result.error.empty()) {
        return;
    }

    rusage before{}, after{};
    if (getrusage(RUSAGE_THREAD, &before) != 0) {
        result.error = "getrusage before workload failed";
        return;
    }
    const auto begin = Clock::now();
    result.startCpu = sched_getcpu();
    int previousCpu = result.startCpu;
    std::vector<double> gaps;
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    auto checkpoint = begin;

    while (Clock::now() < deadline) {
        // A deterministic CPU-bound workload with observable checksum.
        for (unsigned i = 0; i < 4096; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
        }
        ++result.chunks;
        if (result.chunks % 32 == 0) {
            const auto now = Clock::now();
            gaps.push_back(std::chrono::duration<double, std::micro>(now - checkpoint).count());
            checkpoint = now;
            const int currentCpu = sched_getcpu();
            if (currentCpu >= 0 && previousCpu >= 0 && currentCpu != previousCpu) {
                ++result.sampledMigrations;
            }
            previousCpu = currentCpu;
        }
    }

    result.wallSeconds = std::chrono::duration<double>(Clock::now() - begin).count();
    result.lastCpu = sched_getcpu();
    result.checksum = state;
    if (getrusage(RUSAGE_THREAD, &after) != 0) {
        result.error = "getrusage after workload failed";
        return;
    }
    result.cpuSeconds = threadCpuSeconds(after) - threadCpuSeconds(before);
    result.voluntarySwitches = after.ru_nvcsw - before.ru_nvcsw;
    result.involuntarySwitches = after.ru_nivcsw - before.ru_nivcsw;
    result.gapP50Us = percentile(gaps, 0.50);
    result.gapP95Us = percentile(gaps, 0.95);
    result.gapP99Us = percentile(gaps, 0.99);
}

std::vector<WorkerResult> runOne(std::string_view mode, unsigned durationMs,
                                 unsigned niceBOffset, bool competitor,
                                 const std::vector<int> &cpus)
{
    if (mode == "split" && cpus.size() < 2) {
        throw std::runtime_error("split mode requires at least two CPUs in sched_getaffinity");
    }
    const int cpuA = mode == "free" ? -1 : cpus[0];
    const int cpuB = mode == "free" ? -1 : (mode == "same" ? cpus[0] : cpus[1]);
    const int cpuCompetitor = cpus[0];
    std::vector<WorkerResult> results(competitor ? 3U : 2U);
    results[0].role = "A";
    results[1].role = "B";
    if (competitor) {
        results[2].role = "competitor";
    }

    std::barrier gate(static_cast<std::ptrdiff_t>(results.size() + 1));
    Clock::time_point deadline;
    std::jthread a([&] { doWork(results[0], cpuA, 0, gate, deadline); });
    std::jthread b([&] { doWork(results[1], cpuB, niceBOffset, gate, deadline); });
    std::optional<std::jthread> extra;
    if (competitor) {
        extra.emplace([&] { doWork(results[2], cpuCompetitor, 0, gate, deadline); });
    }
    deadline = Clock::now() + std::chrono::milliseconds(durationMs);
    gate.arrive_and_wait();
    a.join();
    b.join();
    if (extra) {
        extra->join();
    }
    for (const auto &result : results) {
        if (!result.error.empty()) {
            throw std::runtime_error(result.role + ": " + result.error);
        }
    }
    return results;
}

void printHeader(std::ostream &out)
{
    out << "run,mode,role,allowed_cpu_count,requested_cpu,start_cpu,end_cpu,nice,competitor,"
           "chunks,wall_seconds,cpu_seconds,voluntary_switches,involuntary_switches,"
           "sampled_migrations,checkpoint_gap_p50_us,checkpoint_gap_p95_us,"
           "checkpoint_gap_p99_us,checksum\n";
}

void printResult(std::ostream &out, unsigned run, std::string_view mode,
                 std::size_t cpuCount, bool competitor, const WorkerResult &value)
{
    out << run << ',' << mode << ',' << value.role << ',' << cpuCount << ',' << value.requestedCpu
        << ',' << value.startCpu << ',' << value.lastCpu << ',' << value.nice << ','
        << (competitor ? 1 : 0) << ',' << value.chunks << ',' << std::fixed
        << std::setprecision(6) << value.wallSeconds << ',' << value.cpuSeconds << ','
        << value.voluntarySwitches << ',' << value.involuntarySwitches << ','
        << value.sampledMigrations << ',' << std::setprecision(3) << value.gapP50Us << ','
        << value.gapP95Us << ',' << value.gapP99Us << ',' << value.checksum << '\n';
}

void selfTest()
{
    const auto cpus = allowedCpus();
    for (std::string_view mode : {"free", "same", "split"}) {
        if (mode == "split" && cpus.size() < 2) {
            continue;
        }
        const auto result = runOne(mode, 150, 0, false, cpus);
        if (result.size() != 2 || result[0].chunks == 0 || result[1].chunks == 0 ||
            result[0].wallSeconds <= 0 || result[1].wallSeconds <= 0 ||
            result[0].cpuSeconds < 0 || result[1].cpuSeconds < 0 ||
            result[0].checksum == 0 || result[1].checksum == 0) {
            throw std::runtime_error("self-test failed in mode " + std::string(mode));
        }
        if (mode == "same" && (result[0].startCpu != cpus[0] ||
                               result[1].startCpu != cpus[0])) {
            throw std::runtime_error("affinity self-test failed");
        }
        if (mode == "split" && (result[0].startCpu != cpus[0] ||
                                result[1].startCpu != cpus[1])) {
            throw std::runtime_error("split affinity self-test failed");
        }
    }
    const auto withCompetitor = runOne("same", 150, 0, true, cpus);
    if (withCompetitor.size() != 3 || withCompetitor[2].chunks == 0) {
        throw std::runtime_error("competitor self-test failed");
    }
    std::cout << "Linux scheduler lab self-test: PASS\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parseOptions(argc, argv);
        if (options.selfTest) {
            selfTest();
            return EXIT_SUCCESS;
        }
        const auto cpus = allowedCpus();
        if (options.mode == "split" && cpus.size() < 2) {
            throw std::runtime_error("split mode requires two allowed CPUs");
        }
        std::ofstream file;
        std::ostream* output = &std::cout;
        if (!options.csv.empty()) {
            const auto path = std::filesystem::path(options.csv);
            if (!path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path());
            }
            file.open(path);
            if (!file) {
                throw std::runtime_error("cannot open CSV output: " + options.csv);
            }
            output = &file;
        }
        printHeader(*output);
        std::vector<std::string> modes;
        if (options.mode == "all") {
            modes = {"free", "same"};
            if (cpus.size() >= 2) {
                modes.emplace_back("split");
            } else {
                std::cerr << "Skipping split: only one CPU permitted by affinity mask\n";
            }
        } else {
            modes.push_back(options.mode);
        }
        for (unsigned run = 1; run <= options.repeats; ++run) {
            // Rotate modes to reduce order bias from heat, frequency and host load.
            for (std::size_t index = 0; index < modes.size(); ++index) {
                const auto &mode = modes[(index + run - 1) % modes.size()];
                const auto results = runOne(mode, options.durationMs, options.niceBOffset,
                                            options.competitor, cpus);
                for (const auto &result : results) {
                    printResult(*output, run, mode, cpus.size(), options.competitor, result);
                }
            }
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "Linux scheduler lab failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
