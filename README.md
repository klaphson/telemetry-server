# telemetry-server

An educational Linux telemetry server written in C++20. It accepts fixed-size
binary telemetry frames on TCP port 9000 by default, validates them, and forwards
them through a shared memory ring to a reader process that decodes the records,
appends their binary frames to rotating files, and prints their fields.

## Architecture

Six static libraries, `Config`, `Server`, `Reader`, `Protocol`, `Storage`, and
`Ipc`, are linked into one executable, `telemetry-server`. Both `Server` and
`Reader` use the binary codec provided by `Protocol`; `Reader` uses `Storage` to
persist frames. TCP connections, the shared ring, and the data file use the same
32-byte frames.

```text
TCP clients → Server (parent process) → shared ring → Reader (child process)
                                                       ├→ Storage → active file + archived segments
                                                       └→ stdout
```

`src/main.cpp` parses and validates CLI options through `Config`, then creates
the shared ring and two eventfds and calls `fork()`. It passes the selected port
to the `Server` constructor through `ServerConfig` and the file and storage
settings to the reader through `ReaderConfig`. The parent calls
`Server::run(ring, dataReadyFd, spaceReadyFd, signalFd, readerPid)`; the child calls
`Reader::run(ring, dataReadyFd, spaceReadyFd, readerConfig)` directly.
Both processes run the same executable, without launching a separate Reader
executable through `exec()`.

- `Config` provides `AppConfig` defaults, CLI parsing, validation, and usage text.
  Help and argument errors exit before any shared ring, child process, or listener
  is created.
- `main()` owns the shared ring and eventfd descriptors and waits for the child
  with `waitpid()` when the server returns. It blocks `SIGINT`, `SIGTERM`, and
  `SIGCHLD` before forking and gives the parent a `signalfd` for shutdown and
  child-state notifications.
- `Server` uses nonblocking sockets and `epoll` to accept connections, read
  binary frames, and push frames into the shared ring. `handleEvent()` dispatches
  one event; `run()` owns the wait loop and cleanup.
- `Server::m_clients` stores each client's input buffer. `ClientState` is a
  private nested struct. Disconnection and shutdown close client sockets and
  remove their entries; the destructor closes any remaining client sockets.
- `Buffer`, defined in `Server/include/Buffer.hpp`, holds bytes in `data`, tracks
  the consumed prefix with `offset`, and compacts it when needed. Client input
  uses a 4 KiB compaction threshold.
- `SharedRingBuffer`, provided by `Ipc`, holds up to 4 MiB of frames in an anonymous
  shared mapping created before `fork()`. Exactly one producer pushes frames and
  one consumer pops them. The server pauses ingress at 3 MiB and checks the 1 MiB
  resume threshold on space-ready notifications. The server notifies the reader
  once after every TCP recv() that publishes complete frames, rather than once
  per frame. The reader notifies the server after draining all available frames.
- `Reader` pops complete frames from the ring, decodes them, appends them to
  storage, and prints their fields. It blocks on the data-ready eventfd when idle.
  Invalid IPC frames, eventfd errors, or storage errors cause a failure exit.
- `FileStore`, provided by `Storage`, owns the active file and parent directory
  descriptors. It appends complete frames, recovers an incomplete trailing frame
  on open, rotates files at a configurable size limit, and syncs after a
  configurable number of appended bytes. It also exposes `sync()` for
  `fdatasync()` at clean reader EOF.

The libraries borrow the ring and eventfd descriptors; `main()` owns their
lifetimes. Client connections belong to `Server`.

On `SIGINT` or `SIGTERM`, the server stops accepting clients, closes existing
client connections, and waits for the shared ring to drain. The parent then marks
the producer closed and wakes the reader through the data-ready eventfd. The
reader syncs its data file before logging EOF and returning success. Input that
has not been queued is not part of this drain. The ring's close flag and eventfd
notifications are separate operations performed by the caller.

The server monitors `SIGCHLD` for the reader's PID. A delivered reader exit,
signal kill, or core dump notification stops the server with failure, even if the
reader exited with code zero. This works while idle, without another client
write. Reader stop and continue notifications are logged without initiating
shutdown; notifications for other child PIDs are ignored. If one signal batch
contains both reader termination and a shutdown request, reader failure takes
precedence. `main()` reaps the reader with `waitpid()` after the server returns
and reports a nonzero exit code or terminating signal.

Child-state notifications can merge. The server currently checks only the
delivered notification, so a stop followed by reader death before the server
handles `SIGCHLD` can leave it waiting with a zombie reader. It does not query
the child's actual exit state while running.

If the parent dies before closing the producer, the close flag remains unset
and an idle reader receives no wake-up. The reader inherits blocked `SIGINT` and
`SIGTERM` signals and can remain blocked on the data-ready eventfd, without
performing its final sync.

`main()` configures stdout for line buffering before `fork()`, including when
output is redirected to a file. Buffering individual `<<` insertions until a
newline reduces interleaved partial log lines. Ordering between the two
processes can still vary.

## Project layout

```text
CMakeLists.txt                 Project settings, executable, and tests
src/main.cpp                  Configuration wiring, shared IPC setup, and process management
Config/
    CMakeLists.txt            Config static library
    include/AppConfig.hpp    Runtime settings and defaults
    include/ConfigParser.hpp CLI parsing status and usage API
    src/ConfigParser.cpp     Argument parsing, validation, and usage text
    test/ConfigParserTest.cpp  Catch2 CLI configuration tests
Server/
    CMakeLists.txt            Server static library
    include/Server.hpp        Server interface and client state
    include/Buffer.hpp        Client byte storage, offset tracking, and compaction
    src/Server.cpp            TCP and epoll handling
    test/                     Catch2 client buffer, backpressure, and signal unit tests
Reader/
    CMakeLists.txt            Reader static library
    include/Reader.hpp        Reader interface
    src/Reader.cpp            Shared ring consumption, frame decoding, and persistence
Ipc/
    CMakeLists.txt            Ipc static library
    include/SharedRingBuffer.hpp  Shared ring API and capacity
    include/EventFd.hpp       Eventfd notification helpers
    src/SharedRingBuffer.cpp  Shared mapping and frame queue
    src/EventFd.cpp           Eventfd reads and writes with EINTR handling
    test/SharedRingBufferTest.cpp  Catch2 shared ring and fork tests
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
tests/process_smoke.sh        Binary TCP/shared ring flow and reader-failure integration test
tests/graceful_shutdown.sh    SIGTERM drain, persistence, reader signals, and failure tests
packaging/systemd/telemetry-server.service  systemd service unit for a Linux host
```

## Build and run on Linux

Requirements: a C++20 compiler, CMake 3.24 or newer, Ninja, and Make.
The server uses Linux APIs including `epoll`, `accept4()`, `mmap()`, `eventfd()`, and `signalfd()`.

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

`make run-service` builds the debug executable and starts it in the foreground
as the `telemetry` user through `runuser`, with data stored at
`/var/lib/telemetry-server/telemetry.bin`. It requires root privileges, an existing
`telemetry` account, and a writable data directory. On a native Linux host, create
the account using the command in [Run as a systemd service](#run-as-a-systemd-service)
if needed, then run:

```bash
sudo install -d -o telemetry -g telemetry -m 0750 /var/lib/telemetry-server
sudo make run-service
```

The `telemetry` user must also be able to access the checkout and execute the
built binary. This Make target starts a foreground process; use the systemd unit
below to manage the installed application as a service.

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
| `--quiet` | Off | Suppress per-record Server and Reader logs; startup and errors remain visible |

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
receives complete frames from the shared ring.

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
  after processing the complete frames in the current receive. On a space-ready
  notification, it resumes if pending data is at most 1 MiB, unless it is shutting
  down. The reader sends this notification after draining all currently available
  frames, so resumption can wait until the ring is empty.
- The shared ring holds at most 4 MiB of pending frame data. An enqueue that would
  exceed this hard limit fails and stops the server with an error.
- While ingress is paused, client sockets are temporarily removed from epoll,
  but stay open. This avoids repeated hangup events and preserves unread TCP
  bytes if a sender closes its write side. On resume, clients are registered
  again, and the server reads remaining data before honoring EOF. An explicit
  shutdown still closes clients and may discard unqueued TCP data.

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

Storage also calls `fdatasync()` before rotation, and the reader calls it after
the producer closes and the ring drains. Setting `syncEveryBytes` to zero
disables only byte-based syncing.
The parent directory is synced with `fsync()` after creating an active file and
after renaming a segment. The `FileStore` destructor closes descriptors without
an explicit sync; library callers should call `sync()` before successful shutdown.

A printed record may still be awaiting the next sync and therefore does not
guarantee durability after a system crash or power loss. Opening, appending,
recovery, rotation, or sync failures make the reader exit with failure; the parent
reports `[main] reader exited code=1`. If the reader is killed by a signal, the
parent instead reports `[main] reader terminated by signal=N`, where `N` is the
signal number.

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
- Client buffer compaction and preservation of unread bytes.
- Shared ring frame preservation, visibility across `fork()`, and draining after
  producer closure.
- File storage: exact bytes and size after appending two frames, removal of
  an incomplete trailing frame on open, and rotation before a third frame exceeds
  a two-frame segment limit, checking the active and archived file sizes.
- Server backpressure: pause/resume, exact binary frame preservation, partial
  frames across a pause, stopping before the next receive, shutdown, and error
  propagation, using a shared ring, eventfds, and nonblocking socket pairs.
  Tests also check batched data-ready notifications, incomplete TCP frames,
  valid frames before protocol errors, buffered frames on half-closed clients
  during backpressure, and eventfd error handling.
- Server signals: empty input, repeated shutdown requests, reader termination
  while running or draining, nonterminal reader events, unrelated signals and
  child PIDs, batched notifications, and reader-failure precedence over shutdown.

These unit tests do not bind port 9000. Run all Catch2 tests with:

```bash
ctest --test-dir build -R '^(ConfigParserTest|TelemetryCodecTest|FileStoreTest|SharedRingBufferTest|BufferTest|ServerBackpressureTest|ServerSignalTest)\.' --output-on-failure
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

Run just the signal tests after building:

```bash
ctest --test-dir build -R '^ServerSignalTest\.' --output-on-failure
```

CMake uses an installed Catch2 3 package when available; otherwise it downloads
Catch2 v3.8.1 during configuration, which requires network access. Configure with
`-DBUILD_TESTING=OFF` to build without tests or Catch2.

The C++ suite has 53 tests: 51 Catch2 cases (including 12 configuration
cases and seven signal cases) and two process integration tests. The optional
Python benchmark harness adds five more unit tests.
Coverage gaps include merged child-state notifications, unexpected parent death,
and unread-frame preservation during paused disconnects.

The Bash smoke test sends a fragmented binary frame followed by another frame,
checks decoded reader output, then terminates the reader to verify that the parent
reports failure and the terminating signal. The graceful-shutdown test first
stops and continues the reader to verify nonterminal child notifications. It then
sends a valid frame, waits for its output, sends `SIGTERM`, and checks the drain
messages, reader EOF, and successful process exit. It uses a temporary data path
and checks that the file contains exactly 32 bytes after shutdown. Additional runs
verify that killing an idle reader fails without further traffic and that a
storage-open failure reports the reader's exit code. Process-exit waits are bounded
even when the script runs outside CTest. Cleanup removes the temporary file.
The smoke test uses the default data path,
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

The image also creates the `telemetry` user and group and prepares
`/var/lib/telemetry-server` with mode `0750`, owned by that account. To run as this
user with persistent storage, use the following instead of `make run` inside the
container's root shell:

```bash
make run-service
```

Compose mounts the `telemetry-data` named volume at `/var/lib/telemetry-server`,
so the active data file and archived segments written by `make run-service`
persist across container recreation. `make run` uses `telemetry.bin` in the
mounted checkout by default. Both commands run in the foreground; stop the
server with Ctrl+C before switching between them.

```bash
docker compose exec dev bash
```

The Compose configuration does not publish port 9000 to the host, so run the
telemetry example inside the container. Run `make test` there after stopping the
server. `make docker-build` builds the image; `make docker-run` builds and runs
the application in a temporary container.

The `.devcontainer` configuration also supports opening the project in the
provided development container.

The image includes the Docker CLI, Compose, and Buildx. Compose mounts the
host's `/var/run/docker.sock` into the development container, so Docker commands
inside it operate on the host's Docker engine and existing containers:

```bash
docker ps
docker compose version
docker buildx version
```

If an existing container reports `docker: command not found`, run
`bash .devcontainer/setup.sh` inside it to install the missing Docker CLI,
Compose, and Buildx. Check access to the host engine with `docker ps`.

After changing the Dockerfile or socket mount, use **Dev Containers: Rebuild
Container** in VS Code. Bind mount source paths passed to Docker are resolved
on the host; `/workspace` is the path inside the development container. Run
the project's `docker compose up` and `make docker-shell` commands from the host
so that the relative repository mounts resolve correctly.

VS Code extensions and server settings are stored in
`.devcontainer/.local/vscode-server/`; Codex configuration, plugins, and skills
are stored in `.devcontainer/.local/codex/`. Compose mounts these host directories
at `/root/.vscode-server` and `/root/.codex`, so their contents survive container
rebuilds. The local directories are excluded from Git and the Docker build
context. Keep them to retain your installed extensions and plugins.

When applying this configuration to an existing container for the first time,
run the following inside that container before rebuilding. Skip this step if
the directories are already mounted:

```bash
mkdir -p .devcontainer/.local/vscode-server .devcontainer/.local/codex
cp -a /root/.vscode-server/. .devcontainer/.local/vscode-server/
tar -C /root/.codex --exclude=./ipc --exclude=./.tmp -cf - . \
    | tar -C .devcontainer/.local/codex -xf -
```

Then use **Dev Containers: Rebuild Container** in VS Code.

## Run as a systemd service

The unit in `packaging/systemd/telemetry-server.service` runs the server as the
`telemetry` user and group, with the executable at
`/usr/local/bin/telemetry-server`. Use these commands on a Linux host running
systemd, from the repository root. Stop any other server using port 9000 first.

Build a release executable without the test dependencies:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
cmake --build build-release
```

Create the service account once (skip this step if it already exists):

```bash
sudo useradd --system --user-group --no-create-home \
    --home-dir /var/lib/telemetry-server --shell /usr/sbin/nologin telemetry
```

Install the executable and unit, then enable the service at boot and start it:

```bash
sudo install -m 0755 build-release/telemetry-server /usr/local/bin/telemetry-server
sudo install -m 0644 packaging/systemd/telemetry-server.service \
    /etc/systemd/system/telemetry-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now telemetry-server.service
```

The unit uses port 9000, 64 MiB segments, and a 1 MiB sync interval. systemd
creates `/var/lib/telemetry-server` with mode `0750`, owned by the service
account. The reader writes `telemetry.bin` and its archived segments there.
The unit applies a `0027` umask and restricts filesystem writes using
`ProtectSystem=strict`; its state directory remains writable. If you change the
data path, also adjust the unit's writable-directory settings and permissions.

Inspect service status and follow the server and reader logs:

```bash
sudo systemctl status telemetry-server.service
sudo journalctl -u telemetry-server.service -f
```

The unit restarts the application after failures with a two-second delay.
To stop it, allowing up to 30 seconds for shutdown:

```bash
sudo systemctl stop telemetry-server.service
```

After editing the installed unit, run `sudo systemctl daemon-reload` followed by
`sudo systemctl restart telemetry-server.service`. To update the executable,
stop the service, repeat the executable installation command, and start it with
`sudo systemctl start telemetry-server.service`.

## Batched Reader storage writes

The Reader now stages up to **64 KiB (2048 telemetry frames)** and passes
complete frames to `FileStore::appendFrames()`. Under sustained load, this
can replace thousands of 32-byte `write()` calls with a few larger writes.
The Reader also flushes smaller, partially filled batches before waiting on
`eventfd`, so a slow or stopped sender does not leave data stranded in memory.

`FileStore` splits incoming batches at the configured segment boundary
and durability threshold. It still rotates only **before** a new frame would
exceed the segment limit and preserves `fdatasync()` after the configured
number of bytes (rounded to complete frames). Existing `appendFrame()` calls
remain supported.

To measure the effect, compare the same workload on the base commit and
this branch using `make benchmark-matrix` (without `strace`). Then run
`make profile-syscalls` with tracing to compare the number of Reader
`write()` calls, not throughput under tracing. Actual batch sizes depend
on how much data is waiting in the shared ring when Reader drains it.

## TCP end-to-end benchmark

The optional benchmark generates 32-byte telemetry frames using the existing
`Protocol::encode()` and sends them through loopback TCP. After storage and
graceful shutdown, it checks **every record**, including magic, version, size,
sensor, metric, sequential timestamp and value. Verification occurs after
the measured interval, so it does not inflate the throughput timing.

The Linux runner uses `inotify` for file-change notifications and `pidfd`
(if available) to detect process exit. It also checks the file size at least
every 100 ms because Docker Desktop host-mounted filesystems may fail to
deliver inotify notifications. When notifications work, no polling delay
is added. Unlike the previous shell loop, there is no mandatory 50 ms sleep
after each notification. Startup occurs before
the timed interval; the end-to-end measurement includes sender startup,
TCP, Server/Reader processing, shutdown and final synchronization.

Build Release binaries and run the default experiment:

```bash
make benchmark
```

Run one custom experiment using the existing arguments (frames, batch, port):

```bash
bash bench/run_benchmark.sh 500000 128 19000
```

To run three repeats for each batch size and export the individual results:

```bash
bash bench/run_benchmark.sh 500000 128 19000 \
  --batches 1,32,128,512 --repeat 3 \
  --csv build-bench/benchmark_results.csv
```

Or use `make benchmark-matrix`. After every batch, the runner prints
median, minimum and maximum throughput across that batch's repeated runs.
The CSV stores one row per completed run; it is overwritten when another
invocation uses the same path. Build output and results under `build-bench/`
are Git-ignored. The run limit is 1,000,000 frames (32 MB), under the
default segment rotation threshold.

The `--quiet` option disables per-record stdout logging, not errors or
lifecycle logs. The script checks persisted frame contents and Reader EOF.
`sender_frames_per_second` measures delivery into TCP, whereas
`end_to_end_frames_per_second` measures the whole pipeline and shutdown.
The tests depend on standard-library Python 3 and Linux inotify; no extra
Python packages are necessary.

Run `make benchmark-test` for Python correctness tests. CTest also includes
them when Python 3 is available. Repeat measurements under comparable
host load; Docker Desktop and `strace` introduce measurement overhead.

## Profiling system calls with strace

The syscall profiler answers **which syscalls the Server and Reader perform
and how often**. It traces the parent and its forked child separately, without
changing the C++ application:

```bash
make profile-syscalls
```

The target builds the same Release binaries as the benchmark and runs a
**10,000-frame** workload (default batch 128 on TCP port 19000). A small workload
keeps detailed per-process traces manageable. Customize it:

```bash
python3 bench/profile_syscalls.py --frames 20000 --batch 128 \
  --port 19000 --output build-bench/syscall-profile
```

The profiler starts Server and Reader normally, **then attaches strace**
to both process PIDs before sending data. This avoids tracing the initial
fork and makes it easier to diagnose whether tracing changes startup
behavior. The flags `-ff -T -s 0 -yy` write separate `trace.<PID>`
files for Server and Reader; `-T` adds elapsed wall time to each
completed syscall. The summary includes per-process call and error counts,
saved to `syscalls.csv`. The profiler also checks the original order
and exact contents of persisted frames.

For reliable file notifications inside Docker Desktop, the profiler writes
temporary telemetry data to a native Linux directory under `/tmp` instead
of the host-mounted `/workspace`. It removes those temporary telemetry bytes
when finished, but **retains** `trace.<PID>`, `server.log`, `strace.log`
and `syscalls.csv` under `build-bench/syscall-profile/`. This means
syscall counts remain useful, but storage latency is not directly comparable
to benchmark runs using a different filesystem.

The script prints each PID, confirms successful attachment to **both**
processes and displays the trace directory. `--timeout 90` allows a
longer storage wait for slow hosts. An unsuccessful run keeps raw trace files
and shows the most recent syscalls, process states and strace messages
directly in the terminal, so failures are easier to debug.

Outputs:

```text
build-bench/syscall-profile/server.log
build-bench/syscall-profile/strace.log
build-bench/syscall-profile/trace.<server PID>
build-bench/syscall-profile/trace.<reader PID>
build-bench/syscall-profile/syscalls.csv
```

Read the results by comparing **call counts**: after the 64 KiB
Reader batching optimization, Reader `write` calls should be far fewer
than the number of stored frames. The exact count depends on occupancy
of the shared ring, rotation and sync boundaries. Server `recvfrom`
calls correspond to socket reads. `read` also includes eventfd/signalfd activity,
and `write` can include lifecycle logs; inspect the trace's file
descriptor paths (`-yy`) when in doubt.

**Do not interpret `elapsed_seconds` as CPU time.**
`-T` measures wall time inside a syscall, so blocking
`epoll_wait()` or `eventfd_read()` may appear
expensive even when the process is sleeping. `strace` perturbs
scheduling and performance, so never compare throughput under strace with
the normal benchmark. Use `perf` later for CPU hotspots.

The Ubuntu devcontainer's Dockerfile includes `strace`, and Compose
adds `SYS_PTRACE` and `seccomp=unconfined`. If
`strace` reports `Operation not permitted`,
rebuild/recreate the development container with the current Compose
configuration. Do not grant tracing privileges to the production service.

Unit-test the profiler's parser with:

```bash
make benchmark-test
```

## CPU hotspot profiling with perf

The next stage after batched storage writes is to identify CPU-heavy C++
functions rather than guessing which loop to optimize. Profiling uses a
separate **Release** build with debug symbols and frame pointers so perf can
resolve function names and call stacks. This is an optimized build, not a
Debug build. It does not change ordinary Release binaries.

First check whether the Linux kernel permits CPU sampling:

```bash
make profile-cpu-check
```

If this succeeds, build and collect a profile:

```bash
make profile-cpu
```

The harness runs the verified loopback benchmark **15 times with 1,000,000
frames and a batch size of 128**, collecting CPU-clock samples inherited by
child processes (including Server and forked Reader). It uses a user-space
software event, `cpu-clock:u`, and frame-pointer call graphs. The sample
frequency is 499 Hz. This includes Python and the load generator too: filter
or inspect process IDs and `telemetry-server` symbols in the report.

Outputs are under `build-perf/cpu-profile/`:

```text
perf.data           binary samples (read with perf)
report.txt          text report sorted by PID, library and symbol
record.log          profiler and benchmark run log
record_runs.csv     all verified benchmark runs
check.log           permission check diagnostics
```

For a shorter run, or CPU software counters instead of call-graph samples:

```bash
python3 bench/profile_cpu.py --frames 500000 --repeat 5
python3 bench/profile_cpu.py --frames 1000000 --repeat 10 --mode stat
```

The `--mode both` option runs separate sampling and counter workloads;
the measurements are therefore not from one single run. Inspect a full
profile with `perf report --stdio -i build-perf/cpu-profile/perf.data`.
The report uses `--sort pid,dso,symbol`, so Server and Reader can be
distinguished by PID. The binary is built with
`-g -fno-omit-frame-pointer`, while keeping Release optimization.

**Docker Desktop warning:** Linux `perf_event_open` may be blocked or
unsupported by the VM kernel even for root. `SYS_PTRACE` used by strace
does not guarantee access to perf. The check command reports this
explicitly; do not make the production container privileged just to
capture a development profile. Prefer native Ubuntu/Linux when Docker
Desktop blocks sampling. On native Ubuntu install a matching
`linux-tools` / `linux-tools-common` package and check `uname -r`
and `perf --version`. Perf results are diagnostic, not an unprofiled
throughput measurement; compare throughput using `make benchmark-matrix`.

The project-wide development plan is recorded in [ROADMAP.md](ROADMAP.md).

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

Dev Containers check for `pre-commit`, `clang-format-18`, `nano`, `bash-completion`,
the Docker CLI, Compose, and Buildx
and install any missing tools when created, on every container start, and whenever VS Code
attaches. The setup also reinstalls the Git hook each time, so reopening VS Code
or rebuilding the container keeps it ready. Packages are installed only when
missing. The setup also configures nano as Git's editor, and the image and Bash
configuration set `EDITOR`, `VISUAL`, and `GIT_EDITOR` to `nano`, so commit message
editing works after container rebuilds. Run Git commits in the environment where
the tools and hook were installed.
The setup also refreshes `~/.bashrc` from `docker/bashrc`, which loads Bash and
Git tab completion in interactive terminals. Keep shell customizations in
`docker/bashrc` so they survive restarts and rebuilds. After setup repairs an
existing container, open a new terminal or run `source ~/.bashrc` in the current
one to load completion.
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

`Buffer::empty()` and `Buffer::pendingBytes()` use
`[[nodiscard]]` to warn when a caller ignores their results:

```cpp
const bool isEmpty = buffer.empty();
```

The attribute does not change runtime behavior. An explicit cast to `void`,
such as `(void)buffer.empty()`, indicates an intentionally discarded result.
