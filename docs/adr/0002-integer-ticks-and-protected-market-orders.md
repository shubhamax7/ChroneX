# ADR 0002: integer ticks and protected market orders

Status: accepted

## Context

Financial prices require stable equality and ordering. A market order without a
limit can traverse arbitrarily stale or sparse liquidity in a simulator.

## Decision

Represent price as a validated integer tick and quantity as an integer lot.
Model market orders as IOC orders bounded by a non-negative protection band
captured from the best opposite price at command arrival.

## Consequences

The model avoids floating-point ambiguity and makes worst-case traversal
explicit. Bound arithmetic must be overflow-checked. The simulator does not
claim that this policy matches any particular production venue.
