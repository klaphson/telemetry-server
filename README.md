# telemetry-server

An educational Linux telemetry server written in C++20. It accepts fixed-size
binary telemetry frames on TCP port 9000, validates them, and forwards them through
a pipe to a reader process that decodes and prints the records.

## Architecture

Three static libraries, `Server`, `Reader`, and `Protocol`, are linked into one
executable, `telemetry-server`. Both `Server` and `Reader` use the binary codec
provided by `Protocol`. TCP connections and the IPC pipe carry the same 32-byte
frames.

```text
TCP clients → Server (parent process) → pipe → Reader (child process) → stdout
```

`src/main.cpp` creates the pipe and calls `fork()`. The parent calls
`Server::run(pipeWriteFd, signalFd)`; the child calls `Reader::run(pipeReadFd)` directly.
Both processes run the same executable, without launching a separate Reader
executable through `exec()`.

- `main()` owns the pipe descriptors, closes unused ends, and waits for the child
  with `waitpid()` when the server returns. It ignores `SIGPIPE` so a closed reader
  is reported as a write error. It blocks `SIGINT` and `SIGTERM` before forking and
  gives the parent a `signalfd` for shutdown notifications.
- `Server` uses nonblocking sockets and `epoll` to accept connections, read
  binary frames, and write queued data to the pipe. `handleEvent()` dispatches one
  event; `run()` owns the wait loop and cleanup.
- `Server::m_clients` stores each client's input buffer. `ClientState` is a
  private nested struct. Disconnection and shutdown close client sockets and
  remove their entries; the destructor closes any remaining client sockets.
- `Buffer`, defined in `Server/include/Buffer.hpp`, holds bytes in `data`, tracks
  the consumed prefix with `offset`, and compacts it when needed. Client input
  uses a 4 KiB compaction threshold; `IpcQueue` inherits `Buffer` and uses 64 KiB.
- `IpcQueue`, defined in `Server/include/IpcQueue.hpp`, buffers outgoing frames
  and tracks partial writes. It is local to `Server::run()`.
- `Reader` performs blocking pipe reads. Its private `readFrames()` method
  accumulates bytes, decodes complete frames, prints their fields, and compacts
  its pending buffer. Invalid IPC frames or EOF with a partial frame cause a
  failure exit.

The libraries borrow their pipe descriptors; `main()` closes them. Client
connections belong to `Server`.

On `SIGINT` or `SIGTERM`, the server stops accepting clients, closes existing
client connections, and drains its queued IPC data. The parent then closes the
pipe write end so the reader can finish and receive EOF. Input that has not been
queued is not part of this drain.

`main()` configures stdout for line buffering before `fork()`, including when
output is redirected to a file. Log lines are flushed at newlines instead of after
each `<<` insertion, preventing the observed mixing of server and reader log
fragments. Ordering between the two processes can still vary.

## Project layout

```text
CMakeLists.txt                 Project settings, executable, and tests
src/main.cpp                  Pipe creation and process management
Server/
    CMakeLists.txt            Server static library
    include/Server.hpp        Server interface and client state
    include/Buffer.hpp        Shared byte storage, offset tracking, and compaction
    include/IpcQueue.hpp      Outgoing pipe queue
    src/Server.cpp            TCP and epoll handling
    test/                     Catch2 queue and backpressure unit tests
Reader/
    CMakeLists.txt            Reader static library
    include/Reader.hpp        Reader interface
    src/Reader.cpp            Pipe reads, frame assembly, and decoding
Protocol/
    CMakeLists.txt            Protocol static library
    include/TelemetryRecord.hpp  Telemetry data fields and equality comparison
    include/TelemetryCodec.hpp    Binary codec API and frame constants
    src/TelemetryCodec.cpp     Binary encoding, decoding, and error descriptions
    test/TelemetryCodecTest.cpp  Catch2 codec unit tests
tests/process_smoke.sh        Binary TCP/pipe flow and reader-failure integration test
tests/graceful_shutdown.sh    SIGTERM drain and reader EOF integration test
```

## Build and run on Linux

Requirements: a C++20 compiler, CMake 3.24 or newer, Ninja, and Make.
The server uses Linux APIs including `epoll`, `accept4()`, `pipe2()`, and `signalfd()`.

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

With the server running, use another **Bash** terminal on the same host or inside
the same container. These examples use Bash's `/dev/tcp` support. Every frame is
32 bytes, without a newline or separator.

### One complete frame

```bash
exec 3<>/dev/tcp/127.0.0.1/9000
printf '\x54\x4c\x52\x59\x00\x01\x00\x20\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x3f\xf0\x00\x00\x00\x00\x00\x00' >&3
exec 3>&-
```

Expected reader output:

```text
[reader] telemetry: sensor=1 metric=2 timestamp_ns=3 value=1
```

### One frame in two steps

Run both steps in the same Bash session. First, open the connection and send the
first 16 bytes:

```bash
exec 3<>/dev/tcp/127.0.0.1/9000
printf '\x54\x4c\x52\x59\x00\x01\x00\x20\x00\x00\x00\x01\x00\x00\x00\x02' >&3
```

Then send the remaining 16 bytes and close the connection:

```bash
printf '\x00\x00\x00\x00\x00\x00\x00\x03\x3f\xf0\x00\x00\x00\x00\x00\x00' >&3
exec 3>&-
```

The server waits until the complete frame is available before decoding it.

### Two frames with one printf

`%b` interprets the byte escapes in each argument. This sends 64 bytes containing
two frames, with timestamps `3` and `4` and values `1.0` and `2.0`:

```bash
exec 3<>/dev/tcp/127.0.0.1/9000
printf '%b%b' \
    '\x54\x4c\x52\x59\x00\x01\x00\x20\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x03\x3f\xf0\x00\x00\x00\x00\x00\x00' \
    '\x54\x4c\x52\x59\x00\x01\x00\x20\x00\x00\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x04\x40\x00\x00\x00\x00\x00\x00\x00' >&3
exec 3>&-
```

Expected reader output:

```text
[reader] telemetry: sensor=1 metric=2 timestamp_ns=3 value=1
[reader] telemetry: sensor=1 metric=2 timestamp_ns=4 value=2
```

One `printf` does not guarantee one TCP read. The server assembles fragmented
frames and processes multiple complete frames received together. The reader
also handles partial frames and multiple frames in each pipe read.

Keep each quoted byte string on one line. A backslash followed by a newline
**inside single quotes** adds literal bytes `5c 0a`, corrupting the frame.
For example, inserting them after `TLRY` produces `unsupported version`, since
version bytes must be `00 01`. Shell continuation backslashes belong outside the
quotes, as in the two-frame example. Use `>&3` to write to the open connection.

### Limits and errors

- Invalid frame headers disconnect the client with a protocol-error log.
- A client that reaches EOF with an incomplete frame is disconnected and its
  trailing bytes are discarded; the server logs the partial frame.
- The server disconnects clients whose pending input exceeds 64 KiB.
- At 3 MiB of pending IPC data, the server pauses accepting and reading clients
  after processing the complete frames in the current receive. It resumes when
  pending data falls to 1 MiB or below, unless it is shutting down.
- The IPC queue allows at most 4 MiB of pending data. An enqueue that would exceed
  this hard limit fails and stops the server with an error.

## Binary telemetry codec

The API in `Protocol/include/TelemetryCodec.hpp` lives in the
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
#include "TelemetryCodec.hpp"

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
- Shared buffer compaction, IPC queue thresholds, and reuse of consumed capacity.
- Server backpressure: pause/resume, exact binary frame preservation, partial
  frames across a pause, stopping before the next receive, shutdown, and error
  propagation, using nonblocking pipes and socket pairs.

These unit tests do not bind port 9000. Run all Catch2 tests with:

```bash
ctest --test-dir build -R '^(TelemetryCodecTest|IpcQueueTest|ServerBackpressureTest)\.' --output-on-failure
```

`Protocol` and its codec tests are already included by the root CMake setup.
Run just the codec tests after building:

```bash
ctest --test-dir build -R '^TelemetryCodecTest\.' --output-on-failure
```

CMake uses an installed Catch2 3 package when available; otherwise it downloads
Catch2 v3.8.1 during configuration, which requires network access. Configure with
`-DBUILD_TESTING=OFF` to build without tests or Catch2.

The suite currently has 25 tests: 23 Catch2 cases and two process integration tests.
The Bash smoke test sends a fragmented binary frame followed by another frame,
checks decoded reader output, then terminates the reader to verify that the parent
reports failure. The graceful-shutdown test sends a valid frame, waits for its
output, sends `SIGTERM`, and checks the drain messages, reader EOF, and successful
process exit.

The process tests require Linux `/proc`, Bash TCP redirection, and a free port
9000. CTest gives each a 15-second timeout and a shared resource lock so they do
not run concurrently. To check for intermittent process or logging failures:

```bash
ctest --test-dir build --output-on-failure \
    -R '^telemetry_(process_smoke|graceful_shutdown)$' --repeat until-fail:100
```

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

`Buffer::empty()` and `Buffer::pendingBytes()`, inherited by `IpcQueue`, use
`[[nodiscard]]` to warn when a caller ignores their results:

```cpp
const bool isEmpty = queue.empty();
```

The attribute does not change runtime behavior. An explicit cast to `void`,
such as `(void)queue.empty()`, indicates an intentionally discarded result.
