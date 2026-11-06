# ChroneX roadmap

The original research plan has nine technical stages. For delivery, they are
grouped into five user-visible phases and five focused commits.

| Delivery phase | Technical roadmap stages | Exit criterion |
| --- | --- | --- |
| 1. Spec foundation | P0 | Versioned semantics, ADRs, green portable CI scaffold. |
| 2. Correct reference engine | P1-P2 | Oracle, golden/boundary/differential tests, invariants. |
| 3. Evidence-led performance | P3-P4 | Baseline, optimized policy, raw measurements, differential proof. |
| 4. Deterministic pipeline | P5-P7 | SPSC pipeline, journal/replay, recovery, market-data reconstruction. |
| 5. Release quality | P8-P9 | End-to-end evidence, reproducibility guide, limitations, v1.0 review. |

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

The Phase 3 baseline harness and first local Release measurements are complete.
The optimized book (P4) remains outstanding. The checked-in run is an initial
single-machine data point with one repetition per case; it is not enough to
accept or reject a performance change. Before P4 is complete, collect repeated
paired trials, capture raw data and environment, and require event-by-event
comparison with the reference oracle.
