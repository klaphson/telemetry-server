# telemetry-server

An educational Linux telemetry server written in C++20. It accepts
newline-delimited TCP records on port 9000 and forwards them through a pipe to a
reader process that prints them.

## Architecture

Two static libraries, `Server` and `Reader`, are linked into one executable,
`telemetry-server`.

The `Protocol` directory defines a separate static library with a binary telemetry
codec and its unit tests. The server and reader still exchange newline-delimited
text; the binary codec is not yet integrated into the TCP or pipe data path.

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
    test/                     Catch2 queue and backpressure unit tests
Reader/
    CMakeLists.txt            Reader static library
    include/Reader.hpp        Reader interface
    src/Reader.cpp            Pipe reads and line processing
Protocol/
    CMakeLists.txt            Protocol static library
    include/TelemetryRecord.hpp  Telemetry data fields and equality comparison
    include/TelemetryCode.hpp    Binary codec API and frame constants
    src/TelemetryCode.cpp     Binary encoding, decoding, and error descriptions
    test/TelemetryCodecTest.cpp  Catch2 codec unit tests
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

## Binary telemetry codec

The API in `Protocol/include/TelemetryCode.hpp` lives in the
`telemetry::protocol` namespace. `TelemetryRecord` contains `sensorId` and
`metricId` (`std::uint32_t`), `timestampNs` (`std::uint64_t`, in nanoseconds),
and `value` (`double`).

Version 1 uses a fixed 32-byte frame. All multibyte fields are stored in
big-endian order:

| Byte offset | Size (bytes) | Field | Encoding / expected value |
| --- | --- | --- | --- |
| 0 | 4 | Magic | `0x544C5259` (ASCII `TLRY`) |
| 4 | 2 | Version | Unsigned integer, `1` |
| 6 | 2 | Declared frame size | Unsigned integer, `32` |
| 8 | 4 | Sensor ID | Unsigned integer |
| 12 | 4 | Metric ID | Unsigned integer |
| 16 | 8 | Timestamp | Unsigned integer, nanoseconds |
| 24 | 8 | Value | IEEE 754 binary64 bits |

`encode(record)` returns a `Frame` (`std::array<std::byte, 32>`).
The codec requires a 64-bit IEEE 754 `double` and preserves its bit pattern,
including signed zero, infinities, and NaN payloads.

`decode(frame)` accepts a `std::span<const std::byte>` containing exactly one
complete frame, including when its starting address is unaligned. The caller
must assemble fragmented input and separate concatenated frames before decoding.
Validation checks the actual size, magic, version, and declared size in that order,
returning `InvalidSize`, `InvalidMagic`, `UnsupportedVersion`, or
`InvalidDeclaredFrameSize` on failure.

The returned `DecodeResult` contains `record` and `error`; its explicit boolean
conversion is true when `error == DecodeError::None`. `toString(error)` provides
a readable error description. For example:

```cpp
#include "TelemetryCode.hpp"

#include <iostream>

void codecExample()
{
    using namespace telemetry::protocol;
    const TelemetryRecord input{42, 7, 1'000'000'000ULL, 23.5};
    const Frame frame = encode(input);
    const DecodeResult result = decode(frame);
    if (result) {
        std::cout << result.record.value << '\n';
    } else {
        std::cerr << toString(result.error) << '\n';
    }
}
```

Link the `Protocol` CMake target to use the codec and its public include directory.

## Tests

Stop any server using port 9000 before running:

```bash
make test
```

Or, after building:

```bash
ctest --test-dir build --output-on-failure
```

Catch2 unit tests cover:

- The binary codec: known wire bytes, integer boundaries, exact floating-point
  bits, truncated and oversized input, invalid headers, unaligned input, and
  error descriptions.
- IPC queue thresholds and server backpressure: pause/resume, record preservation,
  shutdown, and error propagation, using nonblocking pipes and socket pairs.

These unit tests do not bind port 9000. Run all Catch2 tests with:

```bash
ctest --test-dir build -R '^(TelemetryCodecTest|IpcQueueTest|ServerBackpressureTest)\.' --output-on-failure
```

The root `CMakeLists.txt` currently does not include `Protocol`. To enable the
library and its nine codec test cases, add this line after the Catch2 setup and
before the existing `add_subdirectory(Server)` call:

```cmake
add_subdirectory(Protocol)
```

Then configure, build, and run the codec tests:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --target telemetry_protocol_test
ctest --test-dir build -R '^TelemetryCodecTest\.' --output-on-failure
```

CMake uses an installed Catch2 3 package when available; otherwise it downloads
Catch2 v3.8.1 during configuration, which requires network access. Configure with
`-DBUILD_TESTING=OFF` to build without tests or Catch2.

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
