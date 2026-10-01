#include "AppConfig.hpp"
#include "ConfigParser.hpp"
#include "Reader.hpp"
#include "Server.hpp"

#include <fcntl.h>
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

bool blockShutdownSignals(sigset_t &mask)
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

    // Report a closed reader through write() instead of terminating on SIGPIPE.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    sigset_t shutdownMask{};

    if (!blockShutdownSignals(shutdownMask)) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    // Flush complete lines, including when redirected to a file. Flushing each
    // insertion lets the server and reader interleave fragments of their logs.
    if (std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ) != 0) {
        std::cerr << "[main] failed to configure stdout line buffering\n";
        return EXIT_FAILURE;
    }

    const ReaderConfig readerConfig{.dataFilePath = config.dataFilePath,

                                    .maxSegmentBytes = config.maxSegmentBytes,

                                    .syncEveryBytes = config.syncEveryBytes};

    const ServerConfig serverConfig{.port = config.port};

    int pipeFds[2]{};
    if (pipe2(pipeFds, O_CLOEXEC) == -1) {
        perror("pipe2");
        return EXIT_FAILURE;
    }

    const pid_t readerPid = fork();
    if (readerPid == -1) {
        perror("fork");
        close(pipeFds[0]);
        close(pipeFds[1]);
        return EXIT_FAILURE;
    }

    if (readerPid == 0) {
        close(pipeFds[1]);
        const int result = Reader{}.run(pipeFds[0], readerConfig);
        close(pipeFds[0]);
        std::cout.flush();
        std::cerr.flush();
        _exit(result);
    }

    close(pipeFds[0]);

    const int signalFd = signalfd(-1, &shutdownMask, SFD_NONBLOCK | SFD_CLOEXEC);

    if (signalFd == -1) {
        perror("signalfd");

        close(pipeFds[1]);

        int status = 0;
        while (waitpid(readerPid, &status, 0) == -1) {
            if (errno != EINTR) {
                perror("waitpid");
                break;
            }
        }

        return EXIT_FAILURE;
    }

    const int serverResult = Server{serverConfig}.run(pipeFds[1], signalFd);
    close(signalFd);
    close(pipeFds[1]); // Let the reader drain the pipe and receive EOF.

    int status = 0;
    while (waitpid(readerPid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("waitpid");
            return EXIT_FAILURE;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        std::cerr << "[main] reader failed\n";
        return EXIT_FAILURE;
    }
    return serverResult;
}
