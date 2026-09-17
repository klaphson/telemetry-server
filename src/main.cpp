#include "Reader.hpp"
#include "Server.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>

int main()
{
    // Report a closed reader through write() instead of terminating on SIGPIPE.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
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
    const int serverResult = Server{}.run(pipeFds[1]);
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
