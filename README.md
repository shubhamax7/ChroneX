# ChroneX

ChroneX is a deterministic, event-sourced exchange simulator written in C++20.
It is a portfolio-quality systems project: correctness, replayability, and
measured behavior matter more than premature low-latency claims.

> **Project name:** ChroneX. The codebase and all project artifacts use this
> name exclusively.

## Current status: Phase 2 complete

ChroneX now has its correct, single-threaded reference implementation. It
models one instrument with price-time priority, limit GTC/IOC and protected
market orders, cancel, replace, explicit events, and test-build invariants.
The implementation deliberately uses clear standard containers rather than
performance-oriented storage; optimization belongs to a later phase.

Read these documents in order:

1. [Semantics](docs/SEMANTICS.md): the normative behavior and 20 golden scenarios.
2. [Architecture decisions](docs/adr/): why the first constraints exist.
3. [Research notes](docs/RESEARCH.md): evidence and unresolved questions.
4. [Roadmap](docs/ROADMAP.md): phase boundaries and acceptance criteria.

## Build the scaffold

Requirements: CMake 3.24+, Ninja, and a C++20 compiler. Configure dependencies
with network access on the first build.

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

`release` is an equivalent release preset. The CI matrix validates both Debug
and Release with GCC and Clang. Benchmarking is deliberately disabled until
Phase 3; use the `benchmark` preset and `scripts/reproduce.sh` to run the
baseline suite. See [benchmarking guide](docs/BENCHMARKING.md) for workload
definitions and interpretation limits.

## Verification

The reference engine has 78 GoogleTest cases: semantic golden cases, boundary
rejections, and seeded deterministic command-stream checks. Run them with the
build instructions above. These tests establish the baseline; they do not make
any latency, allocation-free, persistence, or production-readiness claim.

## Scope and non-goals

ChroneX will model a single instrument in-process, beginning with limit GTC,
limit IOC, protected market, cancel, and replace commands. It does not claim
production exchange connectivity, venue realism, lock-free whole-system
behavior, or performance results before those claims are measured and published.

See [LICENSE](LICENSE) for use terms.
