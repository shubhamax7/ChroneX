# ADR 0003: event order is a public contract

Status: accepted

## Context

Tests, later journaling, and replay cannot compare two correct runs if event
ordering is accidental or vague.

## Decision

Every command emits exactly one first disposition (`Ack` or `Reject`), then fill
events in match order, then at most one terminal event. Fills use the resting
maker's price. Event fields are value data and carry command and per-command
event sequence identifiers.

## Consequences

Consumers can compare event streams byte-for-byte once serialization is added.
The engine must avoid nondeterministic output iteration and uninitialized
padding. Any intentional contract change requires a semantics version update.
