# ChroneX

ChroneX is a deterministic, event-sourced exchange simulator written in C++20.
It is a portfolio-quality systems project: correctness, replayability, and
measured behavior matter more than premature low-latency claims.

> **Project name:** ChroneX. The codebase and all project artifacts use this
> name exclusively.

## Phase 1: specification foundation

The repository is intentionally in its first roadmap stage. It contains a
versioned behavior specification, architecture decisions, research boundaries,
and a portable CMake/CI skeleton. There is no matching engine yet. That is
deliberate: implementation starts only once the behavior is unambiguous.

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
Phase 3; enabling it uses `-DCHRONEX_BUILD_BENCHMARKS=ON`.

## Scope and non-goals

ChroneX will model a single instrument in-process, beginning with limit GTC,
limit IOC, protected market, cancel, and replace commands. It does not claim
production exchange connectivity, venue realism, lock-free whole-system
behavior, or performance results before those claims are measured and published.

See [LICENSE](LICENSE) for use terms.
