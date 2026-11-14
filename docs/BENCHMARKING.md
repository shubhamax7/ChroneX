# Benchmarking ChroneX

Phase 3 establishes the reference-book baseline. These measurements describe
this executable, compiler, machine, and generated workload; they are not venue,
network, or production-latency claims.

## Workloads

`chronex_benchmarks` includes these Google Benchmark cases:

- `BM_ReferenceBookBalancedAdds`: pre-generated non-crossing GTC command stream
  at 2,000 and 20,000 commands, including fresh-book construction per batch.
- `BM_ReferenceBookCrossingIoc`: 2,000 resting asks followed by 2,000 fully
  crossing IOC buys.
- `BM_ReferenceBookPartialAndCancel`: 1,000 resting adds, 500 same-price
  quantity decreases, then 500 cancels.
- `BM_ReferenceBookAllocationProbe`: 1,000 unique-price resting adds and the
  number of calls to global `operator new` observed during those commands.
- `BM_OptimizedBookBalancedAdds`: the same balanced-add streams using the
  pooled bitmap book. Construction is paused out of the timed region for both
  balanced-add cases, so these compare command processing after initialization.
- `BM_MatchingPipelineBalancedAdds`: a 4,096-command balanced GTC stream through
  the bounded SPSC pipeline. Thread startup and shutdown are paused out of the
  timed region; submit, queue handoff, matching, and result receipt are timed.
  The `queue_full_retries/command` counter reports input-queue full retries per
  command. This is a throughput workload, not a latency distribution or soak.

Workload construction is outside each timed loop where applicable. The engine
returns an allocated vector of events from each command; therefore the current
allocation figure includes output-vector storage and the standard-library
containers. It is a diagnostic count, not a claim about bytes, allocator cost,
or the engine's steady-state allocation-free behavior. Calls made concurrently
by other threads could affect this process-wide counter; run the allocation
case without competing ChroneX processes.

## Reproduce

Requirements: CMake 3.24+, Ninja, a C++20 compiler, and network access on the
first run to fetch the pinned Google Benchmark v1.9.4 source.

```sh
scripts/reproduce.sh results/<machine>/<date>
```

The script configures a Release benchmark build, builds it, records OS, CPU,
compiler, tool versions, commit and dirty state, then saves raw Google Benchmark
JSON. It does not pin CPU affinity, disable turbo, or change power settings.
Record those controls manually when used. Compare results only when the workload,
compiler flags, machine, and measurement conditions are comparable.
The default filter includes the reference and optimized book policies and the
SPSC pipeline workload, and runs
five repetitions with a 50 ms minimum measurement window per repetition. Pass a
Google Benchmark regular-expression filter as the second argument to narrow
the run. Increase the minimum window for more stable measurements; the default
keeps a full local comparison practical while still retaining every sample.

## Interpretation and known gaps

The reference implementation uses ordered maps, linked FIFO queues, an
unordered live-order index, and per-command event vectors. The optimized policy
uses fixed-capacity pooled nodes, intrusive FIFO queues, dense price-level
arrays, and a hierarchical occupancy bitmap. On the local macOS run recorded in
`results/local-macos/2026-11-07/`, the optimized policy processed the balanced
workload faster in the median: 9.12M versus 7.69M commands/s at 2,000 commands,
and 9.49M versus 6.88M at 20,000. These are three repetitions on one machine;
the OS could not provide reliable CPU-frequency or affinity metadata, so treat
the figures as preliminary. Dense-array construction is excluded from that
comparison and has material time and memory cost at large configured price
ranges. Do not extrapolate this result to other workloads or production
latency. The suite still does not collect hardware counters, capture tail
latency distributions for an asynchronous pipeline, or validate against market
data. Preserve raw output, compare on the same machine and workload, rerun correctness checks,
and retain rejected results.

The first pipeline run is preserved in
`results/local-macos/2026-11-14/`. Its five repetitions measured a median
2.99M commands/s and 0.403 full-queue retries per command. This is a single
workstation baseline: CPU model and frequency were unavailable to the tools,
thread affinity could not be set, and the host reported elevated load. Treat
the numbers as a reproducibility example, not a performance claim.
