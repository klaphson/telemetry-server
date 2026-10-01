# telemetry-server

An educational Linux telemetry server written in C++20. It accepts fixed-size
binary telemetry frames on TCP port 9000 by default, validates them, and forwards them through
a pipe to a reader process that decodes the records, appends their binary frames
to rotating files, and prints their fields.

## Architecture

Five static libraries, `Config`, `Server`, `Reader`, `Protocol`, and `Storage`, are linked
into one executable, `telemetry-server`. Both `Server` and `Reader` use the binary
codec provided by `Protocol`; `Reader` uses `Storage` to persist frames. TCP
connections, the IPC pipe, and the data file use the same 32-byte frames.

```text
TCP clients → Server (parent process) → pipe → Reader (child process)
                                                ├→ Storage → active file + archived segments
                                                └→ stdout
```

`src/main.cpp` parses and validates CLI options through `Config`, then creates
the pipe and calls `fork()`. It passes the selected port to the `Server`
constructor through `ServerConfig` and the file and storage settings to the
reader through `ReaderConfig`. The parent calls
`Server::run(pipeWriteFd, signalFd)`; the child calls
`Reader::run(pipeReadFd, readerConfig)` directly.
Both processes run the same executable, without launching a separate Reader
executable through `exec()`.

- `Config` provides `AppConfig` defaults, CLI parsing, validation, and usage text.
  Help and argument errors exit before any pipe, child process, or listener is created.
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
  accumulates bytes, decodes complete frames, appends them to storage, prints
  their fields, and compacts its pending buffer. Invalid IPC frames, EOF with a
  partial frame, or storage errors cause a failure exit.
- `FileStore`, provided by `Storage`, owns the active file and parent directory
  descriptors. It appends complete frames, recovers an incomplete trailing frame
  on open, rotates files at a configurable size limit, and syncs after a
  configurable number of appended bytes. It also exposes `sync()` for
  `fdatasync()` at clean reader EOF.

The libraries borrow their pipe descriptors; `main()` closes them. Client
connections belong to `Server`.

On `SIGINT` or `SIGTERM`, the server stops accepting clients, closes existing
client connections, and drains its queued IPC data. The parent then closes the
pipe write end so the reader can finish and receive EOF. The reader syncs its
data file before logging EOF and returning success. Input that has not been
queued is not part of this drain.

`main()` configures stdout for line buffering before `fork()`, including when
output is redirected to a file. Log lines are flushed at newlines instead of after
each `<<` insertion, preventing the observed mixing of server and reader log
fragments. Ordering between the two processes can still vary.

## Project layout

```text
CMakeLists.txt                 Project settings, executable, and tests
src/main.cpp                  Configuration wiring, pipe creation, and process management
Config/
    CMakeLists.txt            Config static library
    include/AppConfig.hpp    Runtime settings and defaults
    include/ConfigParser.hpp CLI parsing status and usage API
    src/ConfigParser.cpp     Argument parsing, validation, and usage text
    test/ConfigParserTest.cpp  Catch2 CLI configuration tests
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
    src/Reader.cpp            Pipe reads, frame decoding, and persistence
Protocol/
    CMakeLists.txt            Protocol static library
    include/TelemetryRecord.hpp  Telemetry data fields and equality comparison
    include/TelemetryCodec.hpp    Binary codec API and frame constants
    src/TelemetryCodec.cpp     Binary encoding, decoding, and error descriptions
    test/TelemetryCodecTest.cpp  Catch2 codec unit tests
Storage/
    CMakeLists.txt            Storage static library
    include/FileStore.hpp    Storage configuration, append and sync API
    src/FileStore.cpp        File writes, rotation, syncing, and partial-tail recovery
    test/FileStoreTest.cpp   Catch2 storage unit tests
tests/process_smoke.sh        Binary TCP/pipe flow and reader-failure integration test
tests/graceful_shutdown.sh    SIGTERM drain, reader EOF, and persisted-size test
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

By default, the reader creates or appends to `telemetry.bin` in the current
working directory. To select a different file:

```bash
./build/telemetry-server --data-file /tmp/telemetry.bin
```

The parent directory must already exist. The reader needs permission to read and
search the directory, create and rename files there, and append to the active
file.

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

## Command-line options

Run `./build/telemetry-server --help` (or `-h`) to print usage and exit successfully.
With no arguments, the server uses these defaults:

| Option | Default | Requirements |
| --- | --- | --- |
| `--port PORT` | `9000` | Integer from 1 through 65535 |
| `--data-file PATH` | `telemetry.bin` | Nonempty path; parent directory must already exist |
| `--segment-bytes BYTES` | `67108864` (64 MiB) | At least 32 bytes and a multiple of 32 |
| `--sync-bytes BYTES` | `1048576` (1 MiB) | Zero to disable byte-based syncing, or at most the segment size |

Supply each value as a separate argument, such as `--port 9100`.
The `--port=9100` form and positional arguments are not supported. Numeric
values must be unsigned decimal integers without signs, whitespace, fractions,
or unit suffixes; values that overflow their destination type are rejected.
Byte counts use `std::size_t`.

For example, use port 9100, 8 MiB segments, and a 256 KiB sync interval:

```bash
./build/telemetry-server \
    --port 9100 \
    --data-file /tmp/telemetry.bin \
    --segment-bytes 8388608 \
    --sync-bytes 262144
```

The sync interval need not be a multiple of 32. Storage settings are validated
together after parsing, so either option may appear first. When setting a segment
size below the default 1 MiB sync interval, also lower `--sync-bytes` or set it to
zero. For example, `--segment-bytes 32 --sync-bytes 0` is valid, while
`--segment-bytes 32` alone is not. Disabling byte-based syncing still leaves
syncing on rotation and clean shutdown enabled.

Invalid arguments print a diagnostic and usage to stderr, then exit with failure
before starting the server or reader. Examples of rejected arguments:

| Arguments | Reason |
| --- | --- |
| `--port 0`, `--port 70000` | Port outside the allowed range |
| `--port abc` | Non-numeric port |
| `--segment-bytes 31` | Segment smaller than one 32-byte frame |
| `--segment-bytes 33` | Segment not aligned to 32 bytes |
| `--segment-bytes 32 --sync-bytes 64` | Sync interval exceeds segment size |
| `--unknown` | Unknown option |
| `--port` | Missing value; all four value-taking options require one |
| `--data-file ""` | Empty data file path |

## Send telemetry

With the server running, use another **Bash** terminal on the same host or inside
the same container. These examples use Bash's `/dev/tcp` support. Every frame is
32 bytes, without a newline or separator. They assume the default port 9000;
replace it with the selected port if you started the server with `--port`.

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

## File storage

The active file and archived segments contain consecutive 32-byte wire frames,
including each frame's protocol header, with no extra file header or separators.
The reader validates each frame and appends its original bytes before printing
the telemetry log. Restarting the server with the same path preserves complete
frames and resumes appending, rotating before the next append if needed. New
files are created with mode `0640`, subject to the process umask.

### Configuration and rotation

`FileStore` accepts an optional `FileStoreConfig` in its constructor. The reader
sets it from the CLI configuration: `--segment-bytes` controls `maxSegmentBytes`,
and `--sync-bytes` controls `syncEveryBytes`.

| Setting | Default | Requirements |
| --- | --- | --- |
| `maxSegmentBytes` | 64 MiB (`64 * 1024 * 1024`) | At least 32 bytes and a multiple of 32 |
| `syncEveryBytes` | 1 MiB (`1 * 1024 * 1024`) | Zero to disable byte-based syncing, or at most `maxSegmentBytes` |

`openFile()` rejects invalid configuration. For example, a library caller can
configure smaller segments and a shorter sync interval:

```cpp
FileStore store{FileStoreConfig{
    .maxSegmentBytes = 8 * 1024 * 1024,
    .syncEveryBytes = 256 * 1024,
}};
```

Before appending a frame that would exceed `maxSegmentBytes`, storage syncs and
closes the active file, renames it in the same directory, and creates a new file
at the original path. Frames are never split across segments. A file exactly at
the limit rotates on the next append. An existing file larger than the limit
also rotates on the next append; it is not split into smaller archives.

Archive names have the form
`<filename>.<seconds>.<nanoseconds>.<sequence>.segment`. The timestamp comes from
`CLOCK_REALTIME`, and the sequence starts at zero for each `FileStore` instance.
For example, `telemetry.bin` can become
`telemetry.bin.1700000000.123456789.0.segment`. Archives are retained indefinitely;
the segment limit does not cap total disk usage. Use one writer per data path.

### Recovery and durability

When opening a file whose size is not a multiple of 32, `FileStore` truncates the
trailing incomplete frame, syncs the truncated file, and logs the number of
removed bytes. The active data path must refer to a regular file. Recovery checks
file length only; it does not validate existing complete frames, repair their
contents, or inspect archived segments. Use a dedicated telemetry file for the
data path.

Writes handle partial writes and retry interrupted system calls. After a complete
frame brings the bytes appended since the last successful sync to at least
`syncEveryBytes`, `appendFrame()` calls `fdatasync()` before returning success.
The interval need not be a multiple of 32; it is checked after each complete
frame. This is a byte threshold, not a timer.

Storage also calls `fdatasync()` before rotation, and the reader calls it at clean
pipe EOF. Setting `syncEveryBytes` to zero disables only byte-based syncing.
The parent directory is synced with `fsync()` after creating an active file and
after renaming a segment. The `FileStore` destructor closes descriptors without
an explicit sync; library callers should call `sync()` before successful shutdown.

A printed record may still be awaiting the next sync and therefore does not
guarantee durability after a system crash or power loss. Opening, appending,
recovery, rotation, or sync failures make the reader exit with failure; the parent
reports a failed reader when it exits.

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

- CLI configuration: defaults, runtime options, invalid ports, malformed numbers
  and overflow, undersized or unaligned segments, sync intervals larger than the
  segment (including the default interval), unknown options and positional
  arguments, missing values for every option, and empty data paths. Tests check
  error status and diagnostics, plus valid port and storage boundaries, both
  storage-option orders, disabled syncing, and both help flags.
- The binary codec: known wire bytes, integer boundaries, exact floating-point
  bits, truncated and oversized input, invalid headers, unaligned input, and
  error descriptions.
- Shared buffer compaction, IPC queue thresholds, and reuse of consumed capacity.
- File storage: exact bytes and size after appending two frames, removal of
  an incomplete trailing frame on open, and rotation before a third frame exceeds
  a two-frame segment limit, checking the active and archived file sizes.
- Server backpressure: pause/resume, exact binary frame preservation, partial
  frames across a pause, stopping before the next receive, shutdown, and error
  propagation, using nonblocking pipes and socket pairs.

These unit tests do not bind port 9000. Run all Catch2 tests with:

```bash
ctest --test-dir build -R '^(ConfigParserTest|TelemetryCodecTest|FileStoreTest|IpcQueueTest|ServerBackpressureTest)\.' --output-on-failure
```

Run just the CLI configuration tests after building:

```bash
ctest --test-dir build -R '^ConfigParserTest\.' --output-on-failure
```

`Protocol` and its codec tests are already included by the root CMake setup.
Run just the codec tests after building:

```bash
ctest --test-dir build -R '^TelemetryCodecTest\.' --output-on-failure
```

Run just the storage tests after building:

```bash
ctest --test-dir build -R '^FileStoreTest\.' --output-on-failure
```

CMake uses an installed Catch2 3 package when available; otherwise it downloads
Catch2 v3.8.1 during configuration, which requires network access. Configure with
`-DBUILD_TESTING=OFF` to build without tests or Catch2.

The suite currently has 39 tests: 37 Catch2 cases (including 11 configuration
cases) and two process integration tests.
The Bash smoke test sends a fragmented binary frame followed by another frame,
checks decoded reader output, then terminates the reader to verify that the parent
reports failure. The graceful-shutdown test sends a valid frame, waits for its
output, sends `SIGTERM`, and checks the drain messages, reader EOF, and successful
process exit. It uses a temporary data path and, after successfully waiting for
the server to exit, checks that the file exists and contains exactly 32 bytes.
Cleanup removes that temporary file. The smoke test uses the default data path,
so it creates or appends to `telemetry.bin` in its working directory (`build`
when run through the CTest command above).

The process tests require Linux `/proc`, Bash TCP redirection, `stat -c` support,
and a free port 9000. CTest gives each a 15-second timeout and a shared resource
lock so they do not run concurrently. To check for intermittent process or
logging failures:

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
