# ChroneX release review

Review date: 2026-11-14
Reviewed scope: delivery Phases 1-12 (P0-P16), standalone simulator target
Project version: 0.8.0

## Decision

Phase 12 is complete for the standalone simulator prototype. ChroneX is suitable
for review, local development, and reproducible simulation. It has not been
validated for production deployment or approved as a v1.0.0 release. The version
remains 0.8.0 until the reliability, platform, and operational gaps below are
addressed and independently reviewed.

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
- After Phases 10-11, all 97 GoogleTest cases passed in Debug on 2026-11-14.
  New coverage includes streaming recovery, snapshot restoration, checkpoint
  corruption, compaction/continuation, and the checkpoint-before-truncation
  crash window. After Phase 11, all 97 cases also passed in Release,
  Address/UndefinedBehavior Sanitizers, and ThreadSanitizer on the developer
  workstation. Hosted Linux CI results and Windows checkpoint execution remain
  unobserved.
- The Phase 3 balanced-add comparison is preserved as raw JSON in
  `results/local-macos/2026-11-07/`. It contains three repetitions on one
  machine and excludes construction for the two book-policy cases. It is
  preliminary throughput evidence, not a latency or production claim.
- `scripts/reproduce.sh` builds the benchmark target, runs five repetitions by
  default, and stores raw JSON with host/compiler/commit metadata. It includes
  both the reference and optimized book policies by default.
- The Phase 9 SPSC pipeline workload was run five times on the developer
  workstation on 2026-11-14. Median throughput was 2.99M commands/s with 0.403
  full-queue retries per command. Raw JSON and environment metadata are in
  `results/local-macos/2026-11-14/`; CPU model/frequency were unavailable and
  affinity could not be set, so this is a preliminary local baseline only.
- The installed-package consumer configured, built, and passed locally against
  the 0.8.0 Release install. Hosted Linux package and compiler-matrix results
  remain to be observed.
- CI is configured to run Debug and Release on Ubuntu 24.04 with GCC/Clang and
  macOS 15 with Apple Clang. These hosted results have not yet been observed;
  Windows is not currently a supported platform.

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
  of an incomplete final frame, plus version-marked checksummed book snapshots
  and stopped-pipeline checkpoint/compaction.
- Venue-neutral market-by-price snapshots and contiguous absolute-quantity
  updates.
- Normative matching semantics, ADRs, benchmark instructions, recovery
  behavior, known limitations, and build/test instructions.

## Blockers before v1.0.0 or production use

1. **Journal scale and operations:** checkpoints and journal compaction now
   bound normal restart work. Run large-history startup/compaction soak tests,
   define checkpoint retention/backup and disk-full procedures, and specify
   journal/checkpoint migration before long-lived use.
2. **Concurrency validation:** observe the hosted Linux ThreadSanitizer CI job
   and add longer-duration soak and measured overload tests. Local ThreadSanitizer
   and bounded saturation checks pass, but they do not establish sustained
   production behavior.
3. **Scope boundary:** live venue integration is outside the selected standalone
   simulator target. If ChroneX is later extended to connect to a venue, that
   work needs a named adapter, exchange-specific sequencing/correction rules,
   resynchronization, and captured-feed conformance tests.
4. **Performance evidence:** report pipeline latency distributions, test
   representative order-flow distributions, and compare reference and optimized
   policies under the same measured setup. The new balanced-add pipeline
   benchmark records throughput and queue-full retries, but not per-command
   latency. The pipeline still uses the reference book.
5. **Release engineering:** observe the hosted Linux/macOS build matrix and
   package-consumer job, add Windows validation before claiming Windows support,
   publish supported combinations, and define compatibility policy for the
   journal format and public C++ API. The local package consumer passes, but API
   and binary compatibility are not promised.

## Limitations users must account for

- The engine models one instrument and a deliberately small command set. It is
  not exchange connectivity and does not claim venue behavior.
- `SpscRing` requires exactly one producer and one consumer. The queue is
  bounded; application code must handle `false` as backpressure.
- The binary journal and checkpoint formats are versioned, but no migration or
  automated backup/retention policy is provided. A complete checksum mismatch
  stops recovery instead of skipping data.
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
