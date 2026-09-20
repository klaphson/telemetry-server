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

namespace {

bool blockShutdownSignals(sigset_t& mask)
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

    return sigprocmask(
        SIG_BLOCK,
        &mask,
        nullptr
    ) != -1;
}

} // namespace

int main()
{
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

    std::cout << std::unitbuf;

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
        const int result = Reader{}.run(pipeFds[0]);
        close(pipeFds[0]);
        std::cout.flush();
        std::cerr.flush();
        _exit(result);
    }

    close(pipeFds[0]);

    const int signalFd = signalfd(
        -1,
        &shutdownMask,
        SFD_NONBLOCK | SFD_CLOEXEC);

    if (signalFd == -1)
    {
        perror("signalfd");

        close(pipeFds[1]);

        int status = 0;
        while (waitpid(readerPid, &status, 0) == -1)
        {
            if (errno != EINTR)
            {
                perror("waitpid");
                break;
            }
        }

        return EXIT_FAILURE;
    }

    const int serverResult = Server{}.run(pipeFds[1], signalFd);
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
