# ChroneX roadmap

The original research plan has nine technical stages grouped into five delivery
phases. Phases 6 through 9 are post-plan hardening stages added to address the
documented release blockers.

| Delivery phase | Technical roadmap stages | Exit criterion |
| --- | --- | --- |
| 1. Spec foundation | P0 | Versioned semantics, ADRs, green portable CI scaffold. |
| 2. Correct reference engine | P1-P2 | Oracle, golden/boundary/differential tests, invariants. |
| 3. Evidence-led performance | P3-P4 | Baseline, optimized policy, raw measurements, differential proof. |
| 4. Deterministic pipeline | P5-P7 | SPSC pipeline, journal/replay, recovery, market-data reconstruction. |
| 5. Release quality | P8-P9 | End-to-end evidence, reproducibility guide, limitations, v1.0 review. |
| 6. Journal hardening | P10 (post-plan) | Native record synchronization and exhaustive torn-frame recovery. |
| 7. Concurrency validation | P11 (post-plan) | ThreadSanitizer, million-item queue stress, and pipeline saturation/shutdown proof. |
| 8. Package interoperability | P12 (post-plan) | Installable CMake config/target, public headers, and a downstream consumer build in CI. |
| 9. Pipeline throughput evidence | P13 (post-plan) | Reproducible end-to-end SPSC throughput workload with bounded-queue pressure reporting. |

## Phase 1 acceptance criteria

- C++20 CMake project with debug/release presets and strict warnings on
  ChroneX-owned targets.
- CI matrix for GCC/Clang and debug/release, plus formatting verification.
- Pinned GoogleTest and Google Benchmark declarations.
- Normative semantics with at least 20 golden scenarios.
- Four architecture decisions that prevent accidental semantic drift.
- Research notes that identify decisions and unknowns honestly.

## Phase 2 handoff

Phase 2 is complete. The code now contains POD/value domain types, a simple
reference oracle, invariant checker, 78 golden/boundary/determinism tests, and
Debug/Release verification. Do not add pooling, intrusive lists, lock-free
queues, replay, or benchmarks before the reference behavior is compared against
future alternatives.

## Phase 3 handoff

Phase 3 is complete. P4 adds a fixed-capacity pooled order store, intrusive FIFO
price queues, dense price levels, and a hierarchical occupancy bitmap. Eight
seeded command streams compare emitted events and book state against the
reference after every command; the full Debug suite passes. A paired balanced
add benchmark found a preliminary throughput improvement, with raw results and
environment notes in `results/local-macos/2026-11-07/`. Construction cost for
dense price arrays remains excluded from those timings and must be considered
when choosing a price range and capacity. This single-machine result does not
establish production performance. Phase 4 (P5-P7) can now begin.

## Phase 4 handoff

Phase 4 is complete. P5 adds a bounded SPSC command pipeline with explicit
backpressure and ordered results. P6 adds a versioned checksummed command
journal, write-ahead submission wrapper, replay, torn-tail recovery, and
continued sequencing after restart. P7 adds a venue-neutral market-by-price
snapshot and update reconstructor with sequence-gap detection. Journal sync
depends on operating-system, filesystem, and device guarantees, and feed
translation remains venue-specific. The Debug suite passed all 89 tests; Debug,
Release, and Address/UndefinedBehavior sanitizer verification were recorded
with the phase commit.

## Phase 5 handoff

Phase 5 is complete. P8 closes the pipeline, journal/replay, and depth
reconstruction path with end-to-end recovery evidence, current benchmark
reproduction instructions, and explicit durability and feed limitations. P9
records the release decision and remaining v1.0.0 blockers in
`docs/RELEASE_REVIEW.md`. The project was versioned 0.1.0 for experimental and
educational use; it was not declared production-ready. All 89 local tests passed
in Debug, Release, and Address/UndefinedBehavior sanitizer builds. The CI
workflow is configured for GCC/Clang and Debug/Release; hosted results must be
green before a release tag is distributed.

## Phase 6 handoff (post-plan hardening)

Phase 6 is complete. P10 replaces C++ stream buffering with native append writes
and an explicit per-record sync policy. The default waits for `fsync` on POSIX
or `FlushFileBuffers` on Windows before an append succeeds; new POSIX journal
creation also syncs its containing directory. Recovery coverage now
tests every incomplete-frame byte boundary and resumes from the last complete
record. The project version is 0.2.0. Hardware-level power-loss validation,
filesystem-specific testing, journal checkpoints, and cross-platform Windows
execution remain outside this phase. All 91 tests passed in Debug, Release, and
Address/UndefinedBehavior sanitizer builds.

## Phase 7 handoff (post-plan concurrency validation)

Phase 7 is complete. P11 adds a ThreadSanitizer configure/build preset and CI
job, a one-million-item SPSC producer/consumer stress test, a 5,000-command
pipeline backpressure run, and a deterministic shutdown test with both bounded
queues saturated. All 92 tests passed under ThreadSanitizer locally on macOS,
as well as in Debug, Release, and Address/UndefinedBehavior sanitizer builds.
The CI job runs the same suite on Linux; hosted results remain to be observed.
The project version is 0.3.0. This stage validates the one-producer,
one-consumer topology, not arbitrary multi-producer use or production overload
latency.

## Phase 8 handoff (post-plan package interoperability)

Phase 8 is complete. P12 installs the public headers, static library, license,
and versioned CMake package with the exported `ChroneX::chronex` target. The
package config resolves the Threads dependency for downstream projects. CI
installs the Release build and separately configures, builds, and runs a small
consumer project using only the installed package. The project version is
0.4.0; the hosted result for the new consumer job must be observed before
distribution. This verifies CMake package consumption on the CI platform, not
binary ABI stability or support for every compiler and operating system.

## Phase 9 handoff (post-plan pipeline throughput)

Phase 9 adds P13, a reproducible 4,096-command balanced-add workload through
the asynchronous SPSC matching pipeline. It records aggregate commands per
second and bounded-queue full retries per command, while excluding worker
thread startup and shutdown. The standard reproduction script now includes
this workload by default. Five local repetitions measured a median 2.99M
commands/s with 0.403 queue-full retries per command; raw data and limitations
are recorded under `results/local-macos/2026-11-14/`. This provides an initial
end-to-end throughput baseline; latency percentiles, representative production
flow mixes, journal sync cost, and sustained overload behavior remain open. The
project version is 0.5.0.
