# Benchmarking ChroneX

Phase 3 establishes the reference-book baseline. These measurements describe
this executable, compiler, machine, and generated workload; they are not venue,
network, or production-latency claims.

## Workloads

`chronex_benchmarks` contains four Google Benchmark cases:

- `BM_ReferenceBookBalancedAdds`: pre-generated non-crossing GTC command stream
  at 2,000 and 20,000 commands, including fresh-book construction per batch.
- `BM_ReferenceBookCrossingIoc`: 2,000 resting asks followed by 2,000 fully
  crossing IOC buys.
- `BM_ReferenceBookPartialAndCancel`: 1,000 resting adds, 500 same-price
  quantity decreases, then 500 cancels.
- `BM_ReferenceBookAllocationProbe`: 1,000 unique-price resting adds and the
  number of calls to global `operator new` observed during those commands.

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

## Interpretation and known gaps

The reference implementation uses ordered maps, linked FIFO queues, an
unordered live-order index, and per-command event vectors. The suite creates a
baseline for later experiments; it does not yet compare alternative book
policies, collect hardware counters, capture tail latency for an asynchronous
pipeline, or validate against market data. Never infer an optimization win from
one run. Preserve raw output, pair changes on the same seeds, rerun correctness
tests, and retain rejected results.
