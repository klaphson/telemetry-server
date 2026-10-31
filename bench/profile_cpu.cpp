// Standalone C++20 controller for perf. Does not link into the server.
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;
namespace fs = std::filesystem;

namespace
{
struct Options {
    unsigned frames = 1'000'000;
    unsigned batch = 128;
    unsigned port = 19000;
    unsigned repeat = 15;
    unsigned timeout = 300;
    std::string mode = "record";
    fs::path output = "build-perf/cpu-profile";
    std::string perf = "perf";
    std::string python = "python3";
    bool check = false;
    bool selfTest = false;
    bool checkWorkload = false;
};

unsigned parseNumber(std::string_view value, unsigned min, unsigned max, std::string_view name)
{
    unsigned number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number < min ||
        number > max) {
        throw std::runtime_error("invalid --" + std::string{name} + " (expected " +
                                 std::to_string(min) + ".." + std::to_string(max) + ")");
    }
    return number;
}

void printHelp()
{
    std::cout << "Usage: telemetry-profile-cpu [options]\n"
              << "  --check        check perf sampling support only\n"
              << "  --frames N     telemetry records per run (1..1000000; default 1000000)\n"
              << "  --batch N      sender frames per batch (1..4096; default 128)\n"
              << "  --repeat N     benchmark repetitions (1..20; default 15)\n"
              << "  --port N       TCP port (1..65535; default 19000)\n"
              << "  --timeout N    timeout per perf command in seconds (default 300)\n"
              << "  --mode NAME    record, stat, or both (default record)\n"
              << "  --output DIR   report directory (default build-perf/cpu-profile)\n"
              << "  --perf PATH    perf executable (default perf in PATH)\n"
              << "  --python PATH  benchmark Python interpreter (default python3)\n"
              << "  --self-test    run C++ controller tests without perf\n";
}

Options parseOptions(int argc, char* argv[])
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key{argv[i]};
        if (key == "--help" || key == "-h") {
            printHelp();
            std::exit(0);
        }
        if (key == "--check") {
            o.check = true;
            continue;
        }
        if (key == "--self-test") {
            o.selfTest = true;
            continue;
        }
        if (key == "--self-check-workload") {
            o.checkWorkload = true;
            continue;
        }
        if (i + 1 >= argc)
            throw std::runtime_error("missing value for " + std::string{key});
        const std::string value = argv[++i];
        if (key == "--frames")
            o.frames = parseNumber(value, 1, 1'000'000, "frames");
        else if (key == "--batch")
            o.batch = parseNumber(value, 1, 4096, "batch");
        else if (key == "--repeat")
            o.repeat = parseNumber(value, 1, 20, "repeat");
        else if (key == "--port")
            o.port = parseNumber(value, 1, 65535, "port");
        else if (key == "--timeout")
            o.timeout = parseNumber(value, 1, 3600, "timeout");
        else if (key == "--mode") {
            if (value != "record" && value != "stat" && value != "both") {
                throw std::runtime_error("--mode must be record, stat or both");
            }
            o.mode = value;
        } else if (key == "--output")
            o.output = value;
        else if (key == "--perf")
            o.perf = value;
        else if (key == "--python")
            o.python = value;
        else
            throw std::runtime_error("unknown option: " + std::string{key});
    }
    return o;
}

std::string readFile(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string tail(const fs::path &path)
{
    std::ifstream file(path);
    if (!file)
        return "(log not created)";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (lines.size() == 30)
            lines.erase(lines.begin());
        lines.push_back(line);
    }
    std::string result;
    for (const auto &entry : lines)
        result += entry + '\n';
    return result;
}

void runCommand(const std::vector<std::string> &arguments, const fs::path &log,
                unsigned timeoutSeconds)
{
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attr{};
    if (posix_spawn_file_actions_init(&actions) != 0 || posix_spawnattr_init(&attr) != 0) {
        throw std::runtime_error("cannot initialize posix_spawn");
    }
    const std::string logName = log.string();
    int rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logName.c_str(),
                                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rc == 0)
        rc = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (rc == 0)
        rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (rc == 0)
        rc = posix_spawnattr_setpgroup(&attr, 0);
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        throw std::runtime_error("posix_spawn setup failed: " + std::string(std::strerror(rc)));
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &arg : arguments)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    rc = posix_spawnp(&pid, argv.front(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        throw std::runtime_error("cannot start " + arguments.front() + ": " + std::strerror(rc) +
                                 " (install perf or check PATH)");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeoutSeconds};
    int status = 0;
    while (true) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done == -1 && errno != EINTR) {
            kill(-pid, SIGKILL);
            throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(-pid, SIGKILL); // also stop benchmark and its Server/Reader children
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
            }
            throw std::runtime_error("profiling command timed out; log: " + log.string());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        kill(-pid, SIGKILL); // clean up descendants left by a failing benchmark
        const std::string details = tail(log);
        const std::string lowered = details; // full log is available on disk
        std::string hint = "See the log above and inspect the command's output.";
        if (lowered.find("Permission") != std::string::npos ||
            lowered.find("permitted") != std::string::npos ||
            lowered.find("perf_event") != std::string::npos ||
            lowered.find("No permission") != std::string::npos) {
            hint = "perf_event_open is restricted by the Linux kernel. Docker Desktop "
                   "may block it even for root. Check /proc/sys/kernel/perf_event_paranoid "
                   "or run on native Linux; do not add privileges to production.";
        }
        throw std::runtime_error("command failed; log: " + log.string() + "\n" + details + "\n" +
                                 hint);
    }
}

std::vector<std::string> split(std::string_view line)
{
    // Python's csv.DictWriter uses CRLF, while getline removes only LF.
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    std::vector<std::string> result;
    while (true) {
        const auto separator = line.find(',');
        result.emplace_back(line.substr(0, separator));
        if (separator == std::string_view::npos)
            break;
        line.remove_prefix(separator + 1);
    }
    return result;
}

void verifyCsv(const fs::path &path, unsigned frames, unsigned batch, unsigned repeat)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("benchmark did not create " + path.string());
    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error("empty benchmark CSV");
    const auto names = split(line);
    const auto column = [&](std::string_view name) {
        const auto it = std::find(names.begin(), names.end(), name);
        if (it == names.end())
            throw std::runtime_error("CSV missing column " + std::string{name});
        return static_cast<std::size_t>(it - names.begin());
    };
    const auto runCol = column("run"), batchCol = column("batch_frames");
    const auto framesCol = column("frames"), bytesCol = column("bytes");
    const auto verifiedCol = column("verified_frames");
    unsigned n = 0;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        const auto values = split(line);
        if (values.size() != names.size())
            throw std::runtime_error("malformed benchmark CSV row");
        ++n;
        if (n > repeat || values[runCol] != std::to_string(n) ||
            values[batchCol] != std::to_string(batch) ||
            values[framesCol] != std::to_string(frames) ||
            values[bytesCol] != std::to_string(static_cast<std::uint64_t>(frames) * 32) ||
            values[verifiedCol] != std::to_string(frames)) {
            throw std::runtime_error("benchmark CSV verification failed on run " +
                                     std::to_string(n));
        }
    }
    if (n != repeat)
        throw std::runtime_error("benchmark completed only " + std::to_string(n) + "/" +
                                 std::to_string(repeat) + " runs");
}

void selfTest()
{
    if (parseNumber("128", 1, 4096, "batch") != 128)
        throw std::runtime_error("parse error");
    bool rejected = false;
    try {
        (void)parseNumber("0", 1, 4096, "batch");
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    if (!rejected)
        throw std::runtime_error("invalid number was accepted");
    const fs::path path = fs::temp_directory_path() /
                          ("telemetry-profiler-test-" + std::to_string(getpid()) + ".csv");
    try {
        for (const auto* newline : {"\n", "\r\n"}) {
            {
                std::ofstream out(path, std::ios::binary);
                out << "batch_frames,run,frames,bytes,verified_frames" << newline;
                out << "128,1,100,3200,100" << newline << "128,2,100,3200,100" << newline;
            }
            verifyCsv(path, 100, 128, 2);
            rejected = false;
            try {
                verifyCsv(path, 100, 128, 3);
            } catch (const std::runtime_error &) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("partial benchmark was accepted");
            rejected = false;
            try {
                verifyCsv(path, 101, 128, 2);
            } catch (const std::runtime_error &) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("incorrect frame count was accepted");
        }
        fs::remove(path);
    } catch (...) {
        fs::remove(path);
        throw;
    }
    std::cout << "C++ profiler controller self-test: PASS\n";
}

void checkWorkload()
{
    // CPU work to ensure the kernel can actually sample a running program.
    std::uint64_t accumulator = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{350};
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::uint64_t i = 0; i < 5000; ++i)
            accumulator += (i * 31) ^ (accumulator >> 3);
    }
    std::cout << "workload_checksum=" << accumulator << '\n';
}

void checkPerf(const Options &options, const fs::path &executable)
{
    const fs::path data = options.output / "check.data";
    const fs::path log = options.output / "check.log";
    try {
        runCommand({options.perf, "record", "-e", "cpu-clock:u", "-F", "99", "-o", data.string(),
                    "--", executable.string(), "--self-check-workload"},
                   log, 20);
    } catch (...) {
        fs::remove(data);
        throw;
    }
    fs::remove(data);
    std::cout << "perf sampling check: OK\n";
}

void runProfile(const Options &o, const fs::path &executable)
{
    fs::create_directories(o.output);
    checkPerf(o, executable);
    if (o.check)
        return;

    const fs::path root = fs::current_path();
    const fs::path benchmark = root / "bench/run_benchmark.py";
    const fs::path server = root / "build-perf/telemetry-server";
    const fs::path sender = root / "build-perf/telemetry-load";
    if (!fs::is_regular_file(benchmark)) {
        throw std::runtime_error("run from the repository root (missing bench/run_benchmark.py)");
    }
    if (!fs::is_regular_file(server) || !fs::is_regular_file(sender)) {
        throw std::runtime_error("missing build-perf binaries; run make profile-cpu-build");
    }
    if (setenv("SERVER_BIN", server.c_str(), 1) != 0 ||
        setenv("SENDER_BIN", sender.c_str(), 1) != 0) {
        throw std::runtime_error("setenv for benchmark binaries failed");
    }
    const auto runMode = [&](std::string_view mode) {
        const fs::path csv = o.output / (std::string{mode} + "_runs.csv");
        fs::remove(csv);
        std::vector<std::string> command{o.perf, std::string{mode}};
        if (mode == "record") {
            const fs::path data = o.output / "perf.data";
            fs::remove(data);
            command.insert(command.end(), {"-e", "cpu-clock:u", "-F", "499", "--call-graph", "fp",
                                           "-o", data.string()});
        } else {
            const fs::path stats = o.output / "stat.txt";
            fs::remove(stats);
            command.insert(command.end(),
                           {"-e", "task-clock,context-switches,page-faults", "-o", stats.string()});
        }
        command.insert(command.end(), {"--", o.python, benchmark.string(), std::to_string(o.frames),
                                       std::to_string(o.batch), std::to_string(o.port), "--repeat",
                                       std::to_string(o.repeat), "--csv", csv.string()});
        std::cout << "Running perf " << mode << ": " << o.repeat << " x " << o.frames << " frames\n"
                  << std::flush;
        runCommand(command, o.output / (std::string{mode} + ".log"), o.timeout);
        verifyCsv(csv, o.frames, o.batch, o.repeat);
        if (mode == "record") {
            const fs::path data = o.output / "perf.data";
            if (!fs::exists(data) || fs::file_size(data) == 0) {
                throw std::runtime_error("perf.data missing or empty");
            }
            const fs::path report = o.output / "report.txt";
            runCommand({o.perf, "report", "--stdio", "--no-children", "-i", data.string(), "--sort",
                        "pid,dso,symbol"},
                       report, 60);
            if (readFile(report).find("no samples") != std::string::npos) {
                throw std::runtime_error("no CPU samples collected; try more repeats");
            }
            std::cout << "CPU hotspot report: " << report << '\n';
        } else {
            std::cout << "CPU counters:\n" << tail(o.output / "stat.txt") << '\n';
        }
        std::cout << "Verified CSV: " << csv << '\n';
    };
    if (o.mode != "stat")
        runMode("record");
    if (o.mode != "record")
        runMode("stat");
    std::cout << "Samples include Python benchmark and sender too. Compare "
                 "telemetry-server PIDs in report.txt.\n"
                 "Profiling perturbs timings: use make benchmark-matrix for throughput.\n";
}
} // namespace

int main(int argc, char* argv[])
{
    try {
        const Options options = parseOptions(argc, argv);
        if (options.selfTest) {
            selfTest();
            return 0;
        }
        if (options.checkWorkload) {
            checkWorkload();
            return 0;
        }
        const fs::path executable = fs::canonical(argv[0]);
        runProfile(options, executable);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "CPU profiling failed: " << error.what() << '\n';
        return 1;
    }
}
