# Telemetry Server — development roadmap

This is the proposed order of work after optimized batched writes.
Stages are incremental: features marked as existing below already have
basic implementations and should be hardened, not rewritten blindly.
Measure before and after performance changes, and keep changes on
feature branches until tests pass.

| Stage | Focus | Definition of done |
| --- | --- | --- |
| **21** | **CPU sampling with perf (current)** | Optimized symbolized build, repeatable recorded workload, readable report; actionable diagnostics when kernel denies perf |
| 22 | Profile-driven CPU optimization | Identify the top 1–3 CPU hotspots, change only where measured, preserve benchmark correctness |
| 23 | Multi-client TCP | Tests with simultaneous fast/slow senders, fragmented frames, disconnects, no starvation, no unbounded memory |
| 24 | Reader lifecycle recovery | Handle SIGCHLD coalescing, unexpected Reader exit, parent death, bounded shutdown and no orphan/hung child |
| 25 | Crash consistency and storage | Power-loss assumptions, partial tails, archive rotation, fdatasync/fsync semantics, restart and disk-full tests |
| 26 | Runtime observability | Counters for frames/s, drops, active clients, ring occupancy, backpressure, persistence and errors |
| 27 | Stability, fuzz and stress | Address/UB sanitizers, malformed inputs, long-running load, chaos/restart tests, reproducible regressions |
| 28 | CI and static analysis | GitHub Actions for build, Catch2, Python and process tests, clang-format, clang-tidy and sanitizers |
| 29 | Security and configuration | Resource ceilings, fd limits, port exposure, privilege separation, secret-free config and threat model |
| 30 | Service operations | Improve existing systemd/devcontainer support, readiness, signal handling, logs, deployment documentation |
| 31 | Architecture documentation | Dataflow diagrams, format/protocol contract, IPC memory ordering, failure modes and design tradeoffs |
| 32 | v1.0 readiness | Stable documented CLI and file format, release checklist, changelog, reproducible build and release |

## Existing baselines

- 32-byte versioned TCP telemetry frames; Linux epoll, shared mmap ring,
  two eventfds and a forked Reader.
- Rotating durable FileStore and a graceful shutdown path already exist.
  Both need more adversarial and recovery-oriented testing.
- Reader storage writes are batched up to 64 KiB.
- Validated loopback benchmark baseline (batch 128, 1 million frames,
  5 repeated runs): median approximately **11.32 million frames/s**.
  This is environment-specific and not a production throughput guarantee.
- Existing profiler showed Reader write syscalls fall from 10,002 to 109
  for 10,000 input frames. The count includes non-storage writes.
- The user has confirmed `make test` and `make benchmark-test` pass
  on the batched-write version.

## Prioritization rules

1. **Correctness and durability outweigh peak throughput.** A failing
   regression, potential data loss, orphaned process or uncontrolled
   resource use blocks the next optimization.
2. **Only optimize measured hotspots.** Record identical unprofiled
   workloads on comparable hardware before and after any performance patch.
3. **Keep changes reviewable.** Prefer one coherent commit per feature
   branch; merge to main after running the full test suite.
4. **Separate perf diagnostic runs from throughput runs.** Perf, strace,
   debug symbols and virtualization can skew or prevent measurements.
