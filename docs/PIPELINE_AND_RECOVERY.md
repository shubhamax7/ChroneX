# ChroneX pipeline, journal, and depth reconstruction

This document describes the Phase 4 implementation. It does not add venue
semantics to `SEMANTICS.md`.

## Command pipeline

`SpscRing<T, N>` is a bounded ring for one producer and one consumer. Each side
owns one monotonically increasing index on a separate cache line. A producer
publishes a constructed slot with a release store; the consumer observes it
with an acquire load. A full queue returns `false`; callers must retry or
explicitly apply backpressure. Calling either side from multiple threads is
outside the contract.

`MatchingPipeline<N>` owns the `ReferenceBook` on one worker thread. The caller
submits contiguous commands and polls one result per command. Result order
matches command order. `try_submit` returning `false` means that the command
was not accepted. `drain_and_stop()` closes submission, drains accepted work,
joins the worker, and returns results that the caller did not poll. Keep using
the caller thread as the sole producer and result consumer.

`JournaledMatchingPipeline<N>` writes and synchronizes a command before
publishing it to the input queue. If the queue is already full, it returns
`false` without appending. On construction it replays the journal to rebuild
book state and continue at the next sequence number. It uses the portable
`ReferenceBook` as its replay and matching implementation.

## Journal format and recovery

Each frame contains the `CXJ1` format marker, a little-endian payload length, a
CRC-32 checksum, and a canonical little-endian command payload. Payloads store
the command sequence, a stable command tag, and only that command's fields;
they do not serialize C++ object memory, padding, pointers, or `std::variant`
layout. The reader checks frame bounds, version marker, checksum, command tag,
side value, payload size, and contiguous sequences.

When opening for append, an incomplete final frame is truncated to the last
complete checksum-valid command. A checksum failure in a complete frame is a
hard error: replay does not silently skip it. Replay processes commands through
the same reference engine and returns the event vector for every command plus
the final resting sides. Deterministic replay assumes the same ChroneX command
semantics and configuration.

The default `JournalSyncPolicy::EachAppend` writes through the native file API
and waits for `fsync` on POSIX or `FlushFileBuffers` on Windows before returning
success. On POSIX, creation of a new journal also synchronizes its containing
directory. `JournalSyncPolicy::None` skips the sync request and should be
used only when the caller accepts weaker recovery guarantees. The journal is
single-writer; concurrent independent journal objects must not append to the
same file. With per-append sync enabled, the parent directory must already
exist. POSIX syncs that directory when it creates a journal file; Windows
flushes the file handle, and does not claim a separately synchronized
directory-entry guarantee.

A successful sync means the operating system accepted the sync request. It
cannot compensate for storage devices, controllers, virtualization layers, or
network filesystems that misreport or weaken persistence. It also does not make
multi-file state transitions atomic. Per-command sync adds producer latency;
measure it on the target filesystem before choosing an acknowledgment policy.
The recovery tests truncate a frame at every byte boundary and verify recovery
continues from the last complete frame. Real power-cut and filesystem fault
injection remain deployment-level validation.

The journal sync happens before the command enters the pipeline queue. If the
process stops after the sync but before `try_submit` returns, recovery may replay
the command even though the caller never observed `true`. Treat that sequence as
an uncertain acknowledgment and reconcile it from the journal before retrying.
Likewise, a sync error has an uncertain on-disk outcome; the writer becomes
poisoned and must be reopened and recovered rather than continuing.

## Market-by-price depth reconstruction

`DepthReconstructor` accepts an absolute depth snapshot followed by contiguous
absolute-quantity level updates. It validates sequence, configured price and
quantity bounds, duplicate snapshot levels, absent-level removals, and the
continuous-book non-crossing invariant. Each update either changes one level
and advances the sequence or throws without changing state. The view reports
all known bid levels (best first), then ask levels (best first), and both best
prices.

This is a venue-neutral reconstruction primitive, not a network feed adapter.
It does not infer exchange-specific snapshot numbering, implied liquidity,
auction states, trade corrections, or recovery from a sequence gap. Integrators
must obtain a fresh valid snapshot after a gap and translate their venue's feed
messages into this absolute-update model.

## Complexity and trade-offs

- SPSC enqueue/dequeue: O(1), bounded storage O(N), one producer and one
  consumer only.
- Journal append: O(command payload size) plus the selected OS sync latency;
  recovery is O(number of commands) time and memory.
- Depth level update: O(log L) time in the number of stored price levels;
  snapshots and returned views are O(L).

The bounded command queue provides explicit backpressure but does not guarantee
latency. The journal retains the complete command history and currently has no
compaction or checkpoint format. Depth updates use ordered maps for transparent
price ordering; this is a reconstruction layer, not the latency-critical
matching representation.
