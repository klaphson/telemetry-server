# telemetry-server

An educational Linux telemetry server written in C++20. It accepts
newline-delimited TCP records on port 9000 and forwards them through a pipe to a
reader process that prints them.

## Architecture

Two static libraries, `Server` and `Reader`, are linked into one executable,
`telemetry-server`.

```text
TCP clients → Server (parent process) → pipe → Reader (child process) → stdout
```

`src/main.cpp` creates the pipe and calls `fork()`. The parent calls
`Server::run(pipeWriteFd)`; the child calls `Reader::run(pipeReadFd)` directly.
Both processes run the same executable, without launching a separate Reader
executable through `exec()`.

- `main()` owns the pipe descriptors, closes unused ends, and waits for the child
  with `waitpid()` when the server returns. It ignores `SIGPIPE` so a closed reader
  is reported as a write error.
- `Server` uses nonblocking sockets and `epoll` to accept connections, read
  records, and write queued data to the pipe. `handleEvent()` dispatches one
  event; `run()` owns the wait loop and cleanup.
- `Server::m_clients` stores each client's input buffer. `ClientState` is a
  private nested struct. Disconnection and shutdown close client sockets and
  remove their entries; the destructor closes any remaining client sockets.
- `IpcQueue`, defined in `Server/include/IpcQueue.hpp`, buffers outgoing records
  and tracks partial writes. It is local to `Server::run()`.
- `Reader` performs blocking pipe reads and assembles complete lines for output.

The libraries borrow their pipe descriptors; `main()` closes them. Client
connections belong to `Server`.

## Project layout

```text
CMakeLists.txt                 Project settings, executable, and tests
src/main.cpp                  Pipe creation and process management
Server/
    CMakeLists.txt            Server static library
    include/Server.hpp        Server interface and client state
    include/IpcQueue.hpp      Outgoing pipe queue
    src/Server.cpp            TCP and epoll handling
Reader/
    CMakeLists.txt            Reader static library
    include/Reader.hpp        Reader interface
    src/Reader.cpp            Pipe reads and line processing
tests/process_smoke.sh        Process and TCP integration test
```

## Build and run on Linux

Requirements: a C++20 compiler, CMake 3.24 or newer, Ninja, and Make.
The server uses Linux APIs including `epoll`, `accept4()`, and `pipe2()`.

From the project root:

```bash
make build
./build/telemetry-server
```

`make run` builds and starts the server in one command. Stop the foreground
application with Ctrl+C.

The equivalent CMake commands are:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

To enable AddressSanitizer and UndefinedBehaviorSanitizer, use a separate build:

```bash
cmake -S . -B build-sanitized -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DTELEMETRY_ENABLE_SANITIZERS=ON
cmake --build build-sanitized
```

## Send telemetry

With the server running, send records from another Bash terminal in the same
host or container:

```bash
printf 'temperature=23\nhumidity=50\n' > /dev/tcp/127.0.0.1/9000
```

The server terminal should show Reader output such as:

```text
[reader] telemetry: temperature=23
[reader] telemetry: humidity=50
```

Records must end with a newline. The server strips a trailing carriage return
from CRLF records and ignores empty lines. It disconnects clients whose input
buffer exceeds 64 KiB. The outgoing pipe queue is limited to 4 MiB; records that
would exceed that limit are dropped with a log message.

## Tests

Stop any server using port 9000 before running:

```bash
make test
```

Or, after building:

```bash
ctest --test-dir build --output-on-failure
```

The Bash smoke test sends fragmented input and multiple records, checks Reader
output, then terminates the Reader to verify that the parent reports failure.
It requires Linux `/proc`, Bash TCP redirection, and a free port 9000. CTest sets
a 15-second timeout.

## Docker development

The included Dockerfile provides the Linux compiler and debugging tools. Start
and enter the development container from the host:

```bash
make docker-shell
```

Inside the container, the repository is mounted at `/workspace`:

```bash
make run
```

Open another terminal in the same container to send telemetry or inspect the
processes:

```bash
docker compose exec dev bash
```

The Compose configuration does not publish port 9000 to the host, so run the
telemetry example inside the container. Run `make test` there after stopping the
server. `make docker-build` builds the image; `make docker-run` builds and runs
the application in a temporary container.

The `.devcontainer` configuration also supports opening the project in the
provided development container.

## Formatting and Git hooks

C++ formatting uses clang-format 18 and the repository's `.clang-format`.
The development image includes clang-format and pre-commit. Rebuild an existing
Compose container from the host to install them:

```bash
docker compose up -d --build dev
```

For native Ubuntu development, install `clang-format-18` and `pre-commit` with
`apt-get install`. Inside the development container (or your native Linux checkout):

```bash
make hooks    # Install the Git pre-commit hook once per checkout
make format   # Format all tracked C/C++ files
```

Dev Containers install the hook automatically when created. Run Git commits in
the environment where the tools and hook were installed.
The hook formats staged C/C++ files before each commit. If it changes files,
review and stage those changes, then commit again. `make format` also exits with
a nonzero status when it changes files; run it again to confirm formatting passes.

## Observe the processes

While the application runs, use a second Linux terminal:

```bash
ps -ef --forest | grep telemetry-server
cat /proc/<PID>/status
ls -l /proc/<PID>/fd
```

To trace process creation and waiting, start the application under `strace`:

```bash
strace -f -e trace=process ./build/telemetry-server
```

## C++ note: `[[nodiscard]]`

`IpcQueue::empty()` and `IpcQueue::pendingBytes()` use `[[nodiscard]]` to warn
when a caller ignores their results:

```cpp
const bool isEmpty = queue.empty();
```

The attribute does not change runtime behavior. An explicit cast to `void`,
such as `(void)queue.empty()`, indicates an intentionally discarded result.
