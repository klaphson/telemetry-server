#include "AppConfig.hpp"
#include "ConfigParser.hpp"
#include "EventFd.hpp"
#include "Reader.hpp"
#include "Server.hpp"
#include "SharedRingBuffer.hpp"

#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{

bool blockManagedSignals(sigset_t &mask)
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

    if (sigaddset(&mask, SIGCHLD) == -1) {
        return false;
    }

    return sigprocmask(SIG_BLOCK, &mask, nullptr) != -1;
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

    if (!blockManagedSignals(signalMask)) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    // Use line-buffered stdout in both processes, including with redirected output.
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
