#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

void print_process_info(std::string_view role)
{
    std::cout
        << "role=" << role
        << " pid=" << ::getpid()
        << " ppid=" << ::getppid()
        << " uid=" << ::getuid()
        << " gid=" << ::getgid()
        << '\n';
}

int run_process_demo()
{
    std::cout << "=== telemetry-server / stage 1: processes ===\n";
    print_process_info("parent-before-fork");
    std::cout.flush();

    const pid_t child_pid = ::fork();

    if (child_pid < 0) {
        std::cerr << "fork() failed: " << std::strerror(errno) << '\n';
        return EXIT_FAILURE;
    }

    if (child_pid == 0) {
        print_process_info("child");
        std::cout << "child: sleeping for 15 seconds; inspect /proc/"
                  << ::getpid() << " while it is alive\n";
        std::cout.flush();

        std::this_thread::sleep_for(std::chrono::seconds(15));

        std::cout << "child: exiting with status 42\n";
        return 42;
    }

    std::cout << "parent: fork() returned child pid=" << child_pid << '\n';
    std::cout << "parent: waiting for child with waitpid()\n";
    std::cout.flush();

    int status = 0;
    const pid_t waited_pid = ::waitpid(child_pid, &status, 0);

    if (waited_pid < 0) {
        std::cerr << "waitpid() failed: " << std::strerror(errno) << '\n';
        return EXIT_FAILURE;
    }

    if (WIFEXITED(status)) {
        std::cout << "parent: child " << waited_pid
                  << " exited normally, exit_code=" << WEXITSTATUS(status)
                  << '\n';
    } else if (WIFSIGNALED(status)) {
        std::cout << "parent: child " << waited_pid
                  << " terminated by signal=" << WTERMSIG(status)
                  << '\n';
    }

    print_process_info("parent-after-wait");
    return EXIT_SUCCESS;
}

int self_test()
{
    return (::getpid() > 1 && ::getppid() > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "--self-test") {
        return self_test();
    }

    return run_process_demo();
}
