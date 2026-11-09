# ChroneX

ChroneX is a deterministic, event-sourced exchange simulator written in C++20.
It is a portfolio-quality systems project: correctness, replayability, and
measured behavior matter more than premature low-latency claims.

> **Project name:** ChroneX. The codebase and all project artifacts use this
> name exclusively.

## Current status: Phase 5 complete

ChroneX includes a deterministic price-time-priority matching engine, an
optimized pooled book policy, a bounded SPSC execution pipeline, a checksummed
command journal with replay and recovery, and a venue-neutral market-by-price
depth reconstructor. It remains an experimental single-instrument simulator;
see the [release review](docs/RELEASE_REVIEW.md) before treating it as a
production component.

Read these documents in order:

1. [Semantics](docs/SEMANTICS.md): the normative behavior and 20 golden scenarios.
2. [Architecture decisions](docs/adr/): why the first constraints exist.
3. [Research notes](docs/RESEARCH.md): evidence and unresolved questions.
4. [Roadmap](docs/ROADMAP.md): phase boundaries and acceptance criteria.
5. [Pipeline and recovery](docs/PIPELINE_AND_RECOVERY.md): concurrency,
   journal format, and depth reconstruction.
6. [Release review](docs/RELEASE_REVIEW.md): verification evidence and release
   blockers.

## Build and verify

Requirements: CMake 3.24+, Ninja, and a C++20 compiler. Configure dependencies
with network access on the first build.

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

`release` is an equivalent release preset. The CI workflow checks Debug and
Release with GCC and Clang, plus Address/UndefinedBehavior sanitizers and
formatting. Run the sanitizer preset locally with:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel
ctest --test-dir build/sanitizers --output-on-failure
```

Use the `benchmark` preset and `scripts/reproduce.sh` to reproduce book
benchmarks. See [benchmarking guide](docs/BENCHMARKING.md) for workload
definitions and interpretation limits.

## Verification

The suite has 89 GoogleTest cases covering semantic and boundary behavior,
seeded differential streams, queue concurrency and backpressure, journal
round-trips and torn-tail recovery, restart replay, and depth reconstruction.
All 89 pass locally in Debug, Release, and Address/UndefinedBehavior sanitizer
builds. These checks do not establish power-loss durability, venue correctness,
tail latency, or production readiness.

## Scope and non-goals

ChroneX models a single instrument in-process. It does not include a live
exchange connection or venue adapter, OS-synced journal durability, journal
compaction/checkpoints, production latency guarantees, or a lock-free whole
system. The matching pipeline uses the reference book; the optimized book is a
separately verified policy and is not yet integrated into that pipeline.

See [LICENSE](LICENSE) for use terms.
