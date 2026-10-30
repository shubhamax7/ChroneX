# ADR 0004: oracle before optimization

Status: accepted

## Context

Optimized matching structures are difficult to inspect and can share a flawed
understanding of the rules. A faster but unverified book is not useful evidence.

## Decision

Phase 2 will build a simple reference oracle and invariant checker before any
pool, intrusive queue, dense level array, bitmap, SPSC pipeline, or benchmark
claim. Future optimized implementations must be compared event-by-event against
the oracle on deterministic command streams.

## Consequences

Early implementation is intentionally not allocation-free or latency-oriented.
The cost is delayed optimization; the benefit is a trustworthy behavioral
baseline and a durable regression oracle.
