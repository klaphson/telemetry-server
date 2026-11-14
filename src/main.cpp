#include "AppConfig.hpp"
#include "ConfigParser.hpp"
#include "EventFd.hpp"
#include "Reader.hpp"
#include "Server.hpp"
#include "SharedRingBuffer.hpp"

#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

namespace
{

bool blockManagedSignals(sigset_t &mask, bool processMode)
{
    if (sigemptyset(&mask) == -1) {
        return false;
    }

    if (sigaddset(&mask, SIGINT) == -1) {
        return false;
    }

    if (sigaddset(&mask, SIGTERM) == -1) {
        return false;
    }

    if (processMode && sigaddset(&mask, SIGCHLD) == -1) {
        return false;
    }

    const int error = pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (error != 0) {
        errno = error;
        return false;
    }

    return true;
}

struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) {}
    ~Descriptor()
    {
        if (fd != -1) {
            close(fd);
        }
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};

int runThreaded(SharedRingBuffer &ring, int dataReadyFd, int spaceReadyFd,
                const sigset_t &signalMask, const ReaderConfig &readerConfig,
                const ServerConfig &serverConfig)
{
    const Descriptor signalFd{signalfd(-1, &signalMask, SFD_NONBLOCK | SFD_CLOEXEC)};
    if (signalFd.fd == -1) {
        perror("signalfd");
        return EXIT_FAILURE;
    }

    const Descriptor readerDoneFd{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    if (readerDoneFd.fd == -1) {
        perror("eventfd readerDone");
        return EXIT_FAILURE;
    }

    std::atomic<int> readerResult{ReaderCompletion::pending};
    const ReaderCompletion completion{readerDoneFd.fd, readerResult};

    try {
        std::jthread reader([&] {
            int result = EXIT_FAILURE;
            try {
                result = Reader{}.run(ring, dataReadyFd, spaceReadyFd, readerConfig);
            } catch (const std::exception &error) {
                std::cerr << "[reader] exception: " << error.what() << '\n';
            } catch (...) {
                std::cerr << "[reader] unknown exception\n";
            }

            readerResult.store(result, std::memory_order_release);
            if (!telemetry::ipc::notifyEvent(readerDoneFd.fd)) {
                std::cerr << "[reader] could not notify server of completion\n";
            }
        });

        int serverResult = EXIT_FAILURE;
        try {
            serverResult = Server{serverConfig}.run(
                ring, dataReadyFd, spaceReadyFd, signalFd.fd, -1, &completion);
        } catch (const std::exception &error) {
            std::cerr << "[server] exception: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "[server] unknown exception\n";
        }

        ring.closeProducer();
        if (!telemetry::ipc::notifyEvent(dataReadyFd)) {
            // A reader still blocked in eventfd_read cannot be joined.
            std::cerr << "[main] failed to notify reader during shutdown\n";
            std::_Exit(EXIT_FAILURE);
        }
        reader.join();

        const int status = readerResult.load(std::memory_order_relaxed);
        if (status != EXIT_SUCCESS) {
            std::cerr << "[main] reader exited code=" << status << '\n';
            return EXIT_FAILURE;
        }
        return serverResult;
    } catch (const std::exception &error) {
        std::cerr << "[main] reader thread: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace telemetry::config;

    AppConfig config;

    const ParseStatus parseStatus = parseArguments(argc, argv, config, std::cerr);

    if (parseStatus == ParseStatus::HelpRequested) {

        printUsage(std::cout, argv[0]);

        return EXIT_SUCCESS;
    }

    if (parseStatus == ParseStatus::Error) {

        printUsage(std::cerr, argv[0]);

        return EXIT_FAILURE;
    }

    sigset_t signalMask{};

    const bool processMode = config.readerMode == ReaderMode::Process;
    if (!blockManagedSignals(signalMask, processMode)) {
        perror("pthread_sigmask");
        return EXIT_FAILURE;
    }

    // Flush complete log lines even when stdout is redirected.
    if (std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ) != 0) {
        std::cerr << "[main] failed to configure stdout line buffering\n";
        return EXIT_FAILURE;
    }

    const ReaderConfig readerConfig{.dataFilePath = config.dataFilePath,

                                    .maxSegmentBytes = config.maxSegmentBytes,

                                    .syncEveryBytes = config.syncEveryBytes,

                                    .quiet = config.quiet};

    const ServerConfig serverConfig{.port = config.port, .quiet = config.quiet};

    SharedRingBuffer ring;

    if (!ring.create()) {
        return EXIT_FAILURE;
    }

    const int dataReadyFd = eventfd(0, EFD_CLOEXEC);

    if (dataReadyFd == -1) {
        perror("eventfd dataReady");
        return EXIT_FAILURE;
    }

    const int spaceReadyFd = eventfd(0, EFD_CLOEXEC);

    if (spaceReadyFd == -1) {
        perror("eventfd spaceReady");
        close(dataReadyFd);
        return EXIT_FAILURE;
    }

    if (!processMode) {
        const int result = runThreaded(ring, dataReadyFd, spaceReadyFd, signalMask,
                                       readerConfig, serverConfig);
        close(dataReadyFd);
        close(spaceReadyFd);
        return result;
    }

    const pid_t readerPid = fork();
    if (readerPid == -1) {
        perror("fork");
        close(dataReadyFd);
        close(spaceReadyFd);
        return EXIT_FAILURE;
    }

    if (readerPid == 0) {
        const int result = Reader{}.run(ring, dataReadyFd, spaceReadyFd, readerConfig);
        close(dataReadyFd);
        close(spaceReadyFd);
        std::cout.flush();
        std::cerr.flush();
        _exit(result);
    }

    const int signalFd = signalfd(-1, &signalMask, SFD_NONBLOCK | SFD_CLOEXEC);

    if (signalFd == -1) {
        perror("signalfd");

        // No server will produce any more frames.
        ring.closeProducer();

        // Try to wake Reader for a graceful exit.
        if (!telemetry::ipc::notifyEvent(dataReadyFd)) {

            // If wake-up fails, terminate the child.
            if (kill(readerPid, SIGKILL) == -1 && errno != ESRCH) {
                perror("kill reader");
            }
        }

        close(dataReadyFd);
        close(spaceReadyFd);

        int status = 0;

        while (waitpid(readerPid, &status, 0) == -1) {
            if (errno != EINTR) {
                perror("waitpid");
                break;
            }
        }

        return EXIT_FAILURE;
    }

    int serverResult =
        Server{serverConfig}.run(ring, dataReadyFd, spaceReadyFd, signalFd, readerPid);

    // Tell Reader that no more frames will be produced.
    ring.closeProducer();

    // Wake Reader so it can drain remaining frames and exit.
    if (!telemetry::ipc::notifyEvent(dataReadyFd)) {
        std::cerr << "[main] failed to notify reader during shutdown\n";

        serverResult = EXIT_FAILURE;

        // Emergency shutdown: Reader may be blocked in eventfd_read().
        if (kill(readerPid, SIGKILL) == -1 && errno != ESRCH) {
            perror("kill reader");
        }
    }

    close(signalFd);
    close(dataReadyFd);
    close(spaceReadyFd);

    int status = 0;
    while (waitpid(readerPid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("waitpid");
            return EXIT_FAILURE;
        }
    }

    if (WIFSIGNALED(status)) {
        std::cerr << "[main] reader terminated by signal=" << WTERMSIG(status) << '\n';

        return EXIT_FAILURE;
    }

    if (!WIFEXITED(status)) {
        std::cerr << "[main] reader terminated unexpectedly\n";

        return EXIT_FAILURE;
    }

    const int readerExitCode = WEXITSTATUS(status);

    if (readerExitCode != EXIT_SUCCESS) {
        std::cerr << "[main] reader exited code=" << readerExitCode << '\n';

        return EXIT_FAILURE;
    }
    return serverResult;
}
