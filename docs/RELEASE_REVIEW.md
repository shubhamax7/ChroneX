# ChroneX release review

Review date: 2026-11-12
Reviewed scope: delivery Phases 1-7 (P0-P11)
Project version: 0.3.0

## Decision

Phase 7 is complete for the educational prototype. ChroneX is suitable for
review, local development, and reproducible experiments. It is not approved as
a production matching engine or as a v1.0.0 release. The version remains 0.3.0
until the production blockers below are addressed and independently reviewed.

## Evidence

- The configured CI workflow builds and tests Debug and Release with GCC and
  Clang, checks C++ formatting, and runs Address/UndefinedBehavior sanitizers.
- On the developer workstation, all 92 GoogleTest cases passed in Debug,
  Release, Address/UndefinedBehavior sanitizer, and ThreadSanitizer
  configurations on 2026-11-12. The concurrency coverage includes a one-million
  item SPSC transfer, a 5,000-command pipeline run, bounded queue saturation,
  and shutdown while the worker is blocked on a full result queue.
  The rest of the suite covers reference semantics, optimized-book differential
  behavior, pipeline draining, journal command round-trip and replay, torn-tail
  recovery at all incomplete-frame byte boundaries, checksum rejection,
  restart continuation, and depth reconstruction.
- The Phase 3 balanced-add comparison is preserved as raw JSON in
  `results/local-macos/2026-11-07/`. It contains three repetitions on one
  machine and excludes construction for the two book-policy cases. It is
  preliminary throughput evidence, not a latency or production claim.
- `scripts/reproduce.sh` builds the benchmark target, runs five repetitions by
  default, and stores raw JSON with host/compiler/commit metadata. It includes
  both the reference and optimized book policies by default.

The local verification is evidence from one workstation, not a substitute for
observed hosted CI runs on all matrix entries. CI should be green for the final
commit before distributing a tagged binary.

## Scope that is implemented

- One-instrument price-time-priority reference matching with integer prices
  and quantities.
- A separate pooled optimized book with seeded differential checks against the
  reference.
- A bounded SPSC command pipeline with explicit queue-full backpressure and
  ordered results.
- A versioned command journal with CRC-32 frame checks, replay, and truncation
  of an incomplete final frame.
- Venue-neutral market-by-price snapshots and contiguous absolute-quantity
  updates.
- Normative matching semantics, ADRs, benchmark instructions, recovery
  behavior, known limitations, and build/test instructions.

## Blockers before v1.0.0 or production use

1. **Journal scale:** recovery loads and replays the full history in memory.
   Add a checkpoint/snapshot format, compaction, version migration, and
   retention policy before long-lived use.
2. **Concurrency validation:** observe the hosted Linux ThreadSanitizer CI job
   and add longer-duration soak and measured overload tests. Local ThreadSanitizer
   and bounded saturation checks pass, but they do not establish sustained
   production behavior.
3. **Feed integration:** implement and test at least one named venue adapter,
   including its snapshot sequence rules, incremental updates, corrections,
   and resynchronization workflow. Current depth reconstruction is deliberately
   venue-neutral.
4. **Performance evidence:** benchmark the pipeline end to end, report latency
   distributions and queue-full behavior, test representative order-flow
   distributions, and compare the reference and optimized policies under the
   same measured setup. The pipeline currently uses the reference book.
5. **Release engineering:** verify hosted CI, add install/export and consumer
   build checks, publish supported platform/compiler combinations, and define
   compatibility policy for the journal format and public C++ API.

## Limitations users must account for

- The engine models one instrument and a deliberately small command set. It is
  not exchange connectivity and does not claim venue behavior.
- `SpscRing` requires exactly one producer and one consumer. The queue is
  bounded; application code must handle `false` as backpressure.
- The binary journal is versioned, but no cross-version migration is provided.
  A complete checksum mismatch stops recovery instead of skipping data.
- Per-record OS sync requests do not override storage hardware or filesystem
  guarantees; power-loss testing on target deployment systems remains open.
- The optimized policy allocates dense arrays from configured price bounds and
  can consume substantial memory. Its benchmark excludes that one-time setup.
- There are no market-data captures, network tests, tail-latency guarantees,
  high-availability features, authentication, or deployment automation.

## Reproduction

From the repository root:

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug

cmake --preset release
cmake --build --preset release --parallel
ctest --preset release

cmake --preset sanitizers
cmake --build --preset sanitizers --parallel
ctest --test-dir build/sanitizers --output-on-failure

cmake --preset thread-sanitizer
cmake --build --preset thread-sanitizer --parallel
ctest --test-dir build/thread-sanitizer --output-on-failure

scripts/reproduce.sh results/<machine>/<date>
```

The benchmark script needs network access the first time Google Benchmark is
fetched. Preserve its raw JSON and environment file with each reported result.
