# ADR 0001: single-owner deterministic core

Status: accepted

## Context

Matching behavior must be reproducible from a command log. Letting multiple
threads mutate the book would make lock acquisition and scheduling part of the
observable trade order.

## Decision

ChroneX will process sequenced commands and mutate book state on one logical
owner. Every public event derives from that sequence and explicit configuration;
the core reads neither wall-clock time nor random state.

## Consequences

This establishes a clear correctness baseline and replay boundary. It also
places a one-core ceiling on the initial engine. Later pipeline stages may move
data between threads, but must preserve the core's command order and event
contract.
