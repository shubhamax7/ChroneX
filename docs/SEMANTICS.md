# ChroneX semantics v0

Status: **normative and implemented by the Phase 2 reference oracle.** This
document defines the behavior that the reference implementation and its golden
tests enforce. It uses the project name ChroneX exclusively.

## 1. Scope and vocabulary

ChroneX models one continuously traded instrument on one logical engine thread.
Commands are processed strictly in increasing `cmd_seq`. The engine has no
wall-clock input, randomness, or externally visible iteration over unordered
containers.

- `Price` is a signed 32-bit integer number of configured ticks. Valid prices
  are in `[1, price_max]`.
- `Qty` is an unsigned 32-bit integer number of lots. Valid command quantity is
  in `[1, qty_max]`.
- `OrderId` is an unsigned 64-bit client-provided identifier. An ID is live
  only while a resting order with that ID exists.
- A buy crosses a sell when `buy_limit >= sell_price`; a sell crosses a buy
  when `sell_limit <= buy_price`.
- Better price has priority. At the same price, lower acceptance sequence has
  priority (FIFO).

There is one price-time-priority book. Every resting order has a positive
remaining quantity, appears in exactly one price-level FIFO queue, and has a
unique live `OrderId`. A continuous book is never locked or crossed after a
command completes: `best_bid < best_ask` when both sides exist.

## 2. Commands

All commands carry a unique, strictly increasing `cmd_seq`. Duplicate or
out-of-order command sequences are input-log errors, outside the book's normal
rejection model.

### Limit GTC

`NewLimitGtc(id, side, price, qty)` validates `id`, `price`, and `qty`. It then
matches against eligible opposite-side resting liquidity. Each execution uses
the **resting maker's price**. Any positive remainder is appended to the FIFO
queue at its limit price with a new acceptance sequence.

### Limit IOC

`NewLimitIoc(id, side, price, qty)` has the same validation and matching rules
as GTC. A positive remainder never rests; it is cancelled after all fills.

### Protected market order

`NewMarket(id, side, qty, protection_ticks)` validates `id`, `qty`, and the
protection band, but has no resting limit. At command start, capture the best
opposite price. A buy may consume prices no greater than
`best_ask_at_arrival + protection_ticks`; a sell may consume prices no less
than `best_bid_at_arrival - protection_ticks`. If the opposite book is empty,
the entire command is cancelled. Any positive remainder is cancelled; market
orders never rest.

`protection_ticks` is non-negative. Calculating the protected bound must detect
integer overflow and reject before mutating state.

### Cancel

`Cancel(id)` removes the live order identified by `id`. It has no fill. A
filled, previously cancelled, or never-live ID is rejected as `UnknownOrder`.

### Replace

`Replace(id, new_price, new_qty)` validates that the ID is live and the new
parameters are valid **before** changing the book.

- Same price with a lower quantity amends the resting order in place and keeps
  its acceptance sequence. A zero result is invalid, not a cancel.
- A price change or quantity increase removes the old order, then processes the
  new parameters as a fresh limit GTC aggressor. It loses time priority and may
  match immediately.

An invalid replacement emits only a rejection and leaves the original order
unchanged.

## 3. Validation and rejection reasons

Commands are validated before book mutation. The initial oracle recognizes:

| Reason | Meaning |
| --- | --- |
| `InvalidPrice` | Limit price is outside `[1, price_max]`. |
| `InvalidQty` | Quantity is zero or exceeds `qty_max`. |
| `PriceOutOfBand` | A protected market bound overflows or is invalid. |
| `DuplicateOrderId` | The supplied ID is currently live. |
| `UnknownOrder` | Cancel or replace targets a non-live ID. |
| `CapacityExceeded` | Reserved for a bounded implementation; the book is unchanged. |

The first two phases do not reuse an ID until its terminal order has been fully
removed. That rule avoids ambiguity; a later version may explicitly change it.

## 4. Event contract

Each accepted command produces events in this exact order:

1. `Ack(cmd_seq)`.
2. Zero or more `Fill(cmd_seq, event_idx, maker_id, taker_id, price, qty)` in
   match order.
3. At most one terminal event: `Rested` for a GTC remainder, or `Cancelled`
   for an IOC/market remainder or an accepted cancel command.

A rejected command produces exactly one `Reject(cmd_seq, reason)` and no other
event. `event_idx` starts at zero for each command and increments for every
post-disposition event. `Ack` and `Reject` are mutually exclusive.

A fill reduces both involved remaining quantities by exactly its `qty`. A fully
filled maker disappears from its queue before the next match. Partial fills do
not move the maker in its queue. Event output is value-based and must contain
no uninitialized padding or addresses.

## 5. Golden scenarios

The following scenarios are executable prose tests. `B` and `S` mean buy and
sell; `GTC`, `IOC`, and `MKT` have the meanings above. Every command is assigned
the next sequence number starting at 1. `A` means `Ack`, `R` means `Rested`,
`C` means `Cancelled`, and `F(maker,taker,price,qty)` means `Fill`.

1. **Empty-book GTC.** `B 10@100 GTC id=1` yields `A, R(1,10@100)`; bid 100 has
   id 1.
2. **Better buy price.** After scenario 1, `B 5@101 GTC id=2` yields `A, R`;
   best bid is 101, then 100.
3. **Crossing limit uses maker price.** With `S 7@101 GTC id=3`, then
   `B 5@105 GTC id=4`, command 4 yields `A, F(3,4,101,5)`; id 3 remains 2@101.
4. **Aggressor remainder rests.** With empty book, `B 9@100 GTC id=1`, then
   `S 12@99 GTC id=2` yields `A, F(1,2,100,9), R(2,3@99)`.
5. **IOC remainder cancels.** With `S 2@101 GTC id=1`, `B 5@105 IOC id=2`
   yields `A, F(1,2,101,2), C(2,3)`; no bid rests.
6. **Non-crossing IOC cancels.** With empty book, `B 5@100 IOC id=1` yields
   `A, C(1,5)`; the book remains empty.
7. **FIFO at one price.** `S 2@100 id=1`, `S 3@100 id=2`, then `B 4@100 id=3`
   fills `F(1,3,100,2)` then `F(2,3,100,2)`; id 2 remains 1.
8. **Price priority precedes FIFO.** `S 2@99 id=1`, `S 2@100 id=2`, then
   `B 3@101 id=3` fills id 1 first, then id 2.
9. **Partial maker preserves priority.** `S 5@100 id=1`, `S 5@100 id=2`, then
   `B 3@100 id=3` leaves id 1 with 2 ahead of id 2.
10. **Cancel live order.** `B 5@100 id=1`, then `Cancel(1)` yields only `A,
    C(1,5)` and removes id 1.
11. **Cancel unknown order.** On an empty book, `Cancel(99)` yields only
    `Reject(UnknownOrder)`.
12. **Duplicate live ID.** After `B 5@100 id=1`, `S 2@101 id=1` yields only
    `Reject(DuplicateOrderId)`; the original bid remains.
13. **Invalid price is atomic.** After `B 5@100 id=1`, `Replace(1, 0, 4)`
    yields only `Reject(InvalidPrice)`; id 1 is still 5@100.
14. **Invalid quantity is atomic.** `B 0@100 id=1` yields only
    `Reject(InvalidQty)` and creates no order.
15. **Same-price decrease keeps FIFO.** `B 5@100 id=1`, `B 5@100 id=2`, then
    `Replace(1,100,3)` retains id 1 before id 2; a later `S 4@100` fills 3
    from id 1 and 1 from id 2.
16. **Quantity increase loses FIFO.** `B 5@100 id=1`, `B 5@100 id=2`, then
    `Replace(1,100,6)` makes id 2 first; a later `S 1@100` fills id 2.
17. **Price-changing replace can match.** `B 5@100 id=1`, `S 3@101 id=2`,
    then `Replace(1,101,5)` fills 3 at maker price 101, then rests id 1 with
    2@101.
18. **Market protection stops traversal.** With `S 2@100 id=1` and
    `S 2@103 id=2`, a `B MKT qty=4 band=1 id=3` fills 2@100 and cancels 2;
    it must not consume 103.
19. **Market on an empty opposite book.** A `B MKT qty=5 band=2 id=1` against
    no asks yields `A, C(1,5)` and does not rest.
20. **No crossed book after command.** For any valid sequence of the preceding
    operations, if both sides are non-empty after a command, assert
    `best_bid < best_ask`; a violation is an oracle defect, not an acceptable
    intermediate state.

## 6. Invariants for the Phase 2 oracle

After every command in a test build, the oracle will check: positive remaining
quantity; live-ID index and book queues agree; each live ID occurs once; FIFO
sequence order at a price; no crossed book; conservation of quantity per order;
and deterministic events for identical command logs and configuration.

## 7. Explicit non-goals

This version excludes auctions, stop/iceberg/pegged orders, self-trade
prevention, multiple instruments, sessions, network protocols, persistence,
and order-ID reuse. Do not infer their semantics from a real exchange.
