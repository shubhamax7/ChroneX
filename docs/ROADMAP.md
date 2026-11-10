# ChroneX roadmap

The original research plan has nine technical stages grouped into five delivery
phases. Phase 6 is a post-plan hardening stage added to address the documented
release blockers.

| Delivery phase | Technical roadmap stages | Exit criterion |
| --- | --- | --- |
| 1. Spec foundation | P0 | Versioned semantics, ADRs, green portable CI scaffold. |
| 2. Correct reference engine | P1-P2 | Oracle, golden/boundary/differential tests, invariants. |
| 3. Evidence-led performance | P3-P4 | Baseline, optimized policy, raw measurements, differential proof. |
| 4. Deterministic pipeline | P5-P7 | SPSC pipeline, journal/replay, recovery, market-data reconstruction. |
| 5. Release quality | P8-P9 | End-to-end evidence, reproducibility guide, limitations, v1.0 review. |
| 6. Journal hardening | P10 (post-plan) | Native record synchronization and exhaustive torn-frame recovery. |

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
execution remain outside this phase. All 91 tests pass in Debug, Release, and
Address/UndefinedBehavior sanitizer builds.
