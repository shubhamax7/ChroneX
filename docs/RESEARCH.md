# ChroneX research notes

Status: **Phase 1 research boundary.** These notes distinguish decisions made
for ChroneX from performance claims. No benchmark number is implied here.

## Chosen evidence-informed constraints

1. **Reference implementation before optimization.** A simple, readable oracle
   makes later differential testing possible. The initial book will favor
   semantic clarity over specialized storage.
2. **Price-time priority is explicit.** Price ordering and FIFO acceptance order
   are observable behavior, not an implementation accident.
3. **Integer price ticks and lots.** Floating-point prices make equality,
   ordering, serialization, and replay unnecessarily ambiguous.
4. **Single logical owner.** Command order, state mutation, and event ordering
   have one owner. Concurrency, persistence, and optimized layout are later
   concerns with separate evidence requirements.
5. **No performance headline before measurement.** CI verifies correctness; it
   is not a benchmark laboratory. Phase 3 will document hardware, compiler,
   flags, workload, warm-up, raw results, and rejected experiments.

## Sources to consult as implementation begins

- CMake and CTest official documentation for preset and test behavior.
- GoogleTest documentation for table-driven golden tests.
- Google Benchmark documentation for later measurement methodology.
- The C++ standard library documentation for ordered containers and integer
  overflow handling.
- Exchange-rule specifications only as references; ChroneX's normative behavior
  remains `SEMANTICS.md`, not an inferred venue rulebook.

## Questions deliberately left open

| Question | Decision point | Why it is open |
| --- | --- | --- |
| Exact reference-book containers | Phase 2 | Correctness tests should choose based on clarity. |
| Order-ID reuse after terminal state | Before Phase 2 implementation | It affects the ID-index contract and events. |
| Event representation and hash | Phase 2 | It must be canonical before replay exists. |
| Optimized level storage | Phase 4 | It needs a Phase 3 baseline and differential proof. |
| Bounded pool failure semantics | Phase 4 | `CapacityExceeded` is specified but no pool exists yet. |
| SPSC topology and backpressure | Phase 5 | It must not weaken deterministic command order. |
| Journal durability guarantee | Phase 6 | It depends on platform I/O semantics and fault tests. |

## Research hygiene

Every later performance or architecture statement must label whether it is a
measured result, a theoretical complexity claim, a design goal, or an
unverified hypothesis. Third-party market data is not committed; a future
download script will record source, checksum, and license constraints.
