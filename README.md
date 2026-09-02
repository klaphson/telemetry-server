# telemetry-server

Educational high-performance Linux telemetry server built incrementally in modern C++.

## Stage 1: processes

This stage demonstrates:

- Linux process identity: PID, PPID, UID and GID
- `fork()`
- parent/child execution
- `waitpid()`
- process exit status
- observing processes through `ps` and `/proc`

## C++ notes: `[[nodiscard]]`

`[[nodiscard]]` (available since C++17) tells the compiler to warn when a caller
ignores a return value. In `src/server_main.cpp`, `IpcQueue::empty()` and
`IpcQueue::pending_bytes()` use this attribute because their results are the
purpose of calling them.

```cpp
IpcQueue queue;
queue.empty();                       // May warn: result is ignored.
const bool is_empty = queue.empty(); // Store the result for later use.
if (!is_empty) {
    // Handle queued data.
}
```

The attribute does not change runtime behavior or guarantee a compilation error.
If ignoring a result is intentional, an explicit cast to `void`, such as
`(void)queue.empty();`, suppresses the nodiscard diagnostic.

C++20 also allows an explanation in the attribute:

```cpp
[[nodiscard("Check whether saving succeeded")]] bool save();
```

## Native build

Requirements:

- Linux
- C++20 compiler
- CMake >= 3.24
- Ninja

```bash
make run
```

## Docker development environment

### Open the workspace directly in the container

In VS Code (or Codex), install the **Dev Containers** extension if needed, then
choose **Dev Containers: Reopen in Container**.  The project will use the `dev`
service from `compose.yaml`; every newly opened integrated terminal will then
start inside the container at `/workspace`.

From a regular host terminal, use `make docker-shell` to enter that same
running container.

Build the toolchain image:

```bash
make docker-build
```

Open a shell in the development container:

```bash
make docker-shell
```

Inside the container:

```bash
make run
```

Or build and run directly:

```bash
make docker-run
```

## Observe the processes

Run the program in terminal A:

```bash
./build/telemetry-server
```

During the child's 15-second sleep, use terminal B:

```bash
ps -ef --forest | grep telemetry-server
```

Inspect a specific process:

```bash
cat /proc/<PID>/status
```

Inspect its open file descriptors:

```bash
ls -l /proc/<PID>/fd
```

Inspect the process tree:

```bash
pstree -p
```

Trace the system calls involved in process creation:

```bash
strace -f -e trace=process ./build/telemetry-server
```

Useful calls to look for:

- `clone(...)` or `clone3(...)` — libc implementation used underneath `fork()` on Linux
- `wait4(...)` / `waitid(...)` — underlying wait operation
- `exit_group(...)` — process termination

## Why Docker is here

Docker gives us a reproducible Linux userspace and toolchain. It does **not** emulate a separate Linux kernel: containers use the host Linux kernel. This is especially useful in this course because `/proc`, processes, signals, namespaces, sockets and system calls remain real Linux kernel mechanisms.

Later stages will add threads, scheduling, IPC, shared memory, sockets, epoll, signals, filesystem layout, permissions and a systemd service.
