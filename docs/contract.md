# Engine contract

This document specifies the target behavior. Limit adds with symmetric matching,
partial fills, field/duplicate/capacity rejection, event sequencing, and read-only
queries and cancellation are implemented. Replace commands
currently throw `std::logic_error` without changing orders or consuming sequence
numbers. This is a temporary development guard, not a venue rejection policy.
Replacement examples below describe future behavior. Matching tests
cover FIFO and partial/full fills on both sides, multi-level sweeps, limit-price
stopping, exhausted-level cleanup, and residual quantity and FIFO placement.

Storage uses ascending price maps (bids traversed in reverse), FIFO lists per
level, and an unordered ID index holding side, price, and stable list iterators.
Map insertions and ID-index rehashing preserve list iterators. Removal erases
the ID entry before its list node and then any empty level.
Insertion costs O(log P) plus expected O(1) ID indexing; best-price queries are
O(1), and depth copies O(min(n, bid levels) + min(n, ask levels)) entries.
Matching visits each filled maker once, with expected O(1) ID erasure and
amortized O(1) iterator-based level erasure, plus insertion for any remainder.
Cancellation uses expected O(1) ID lookup, O(log P) price-level lookup, and
O(1) FIFO unlink, followed by amortized O(1) empty-level erasure. It removes
only the current remaining quantity and preserves all surviving FIFO positions.
Aggregate quantity cannot exceed 10^18 under the enforced count/quantity limits.
Internal storage tests inspect list handles; execution tests verify FIFO through
emitted trades, including a partially filled maker retaining its position.

## Commands and limits

All orders are single-instrument GTC limits. `Add{id, side, price, quantity}`,
`Cancel{id}`, and `Replace{id, price, quantity}` form the `Command` variant.
Replace supplies a new remaining quantity, keeps side and ID, and always resets
FIFO priority. It may immediately trade. Cancel removes the entire remainder.
IDs may be reused after an order leaves the book.

| Value | Accepted range |
| --- | --- |
| ID (`uint64_t`) | 1 through UINT64_MAX; unique among live orders |
| Price (`int64_t`) | 1 through 10^12 integer ticks |
| Submitted remaining quantity (`uint64_t`) | 1 through 10^12 |
| Side | `Side::buy` or `Side::sell` |
| Live orders | At most 1,000,000 |
| Aggregate resting quantity | At most 10^18, derived from the two limits above |

Every add checks capacity BEFORE matching. At full capacity, even an add that
could fully execute is rejected. A replace does not require another live slot.
These conservative limits simplify prevalidation; they are project policy.
The future text parser must reject negative/overflowing unsigned inputs before
conversion. Invalid syntax is a parser error, not an engine command.

## Validation and failure behavior

Field errors take precedence: invalid ID, invalid side (add only), invalid price,
then invalid quantity. After fields pass, add checks duplicate ID then capacity;
cancel/replace checks whether the ID exists. All logical rejections preserve
orders, quantities, and FIFO priority. They still consume one event sequence.
`validate_fields()` is stateless: success does not guarantee engine acceptance.

Rejection codes are `invalid_id`, `invalid_side`, `invalid_price`,
`invalid_quantity`, `duplicate_id`, `unknown_id`, and `capacity_exceeded`.
Replace must finish validation before removing the original order. A valid
replace behaves as cancel followed by add, with one replace acknowledgement.

Sequence starts at 1 and covers ALL events. Before processing each command, the
engine requires room for `max_live_orders + 1` events (one acknowledgement
plus at most one fill per resting order). If insufficient sequence space remains,
throw `std::overflow_error` before mutation instead of wrapping; no event is
emitted. This conservative check also applies to invalid commands. A new book
starts a new sequence; queries consume none.

Allocation failures propagate as `std::bad_alloc`, not business rejections. The
baseline does not promise rollback after allocation failure: stop the run and
destroy the book rather than continue processing. Query allocation failures do
not mutate the book. No performance or no-allocation guarantee exists yet.

## Matching and events

Buys match the lowest ask while limit >= ask; sells match the highest bid while
limit <= bid. Match FIFO at each price and execute at the resting maker's price.
Emit one `Trade` per maker fill, remove exhausted makers and empty levels, and
rest any incoming remainder at the back of its price level.

`process()` returns an owned vector of sequenced events in this exact order:

1. Valid command: one `Accepted{kind, id}`.
2. Add or replace: zero or more `Trade{maker_id, taker_id, price, quantity}` events
   in matching order. Cancel emits only its acknowledgement.
3. Invalid command: one `Rejected{kind, id, reason}` and nothing else.

There are no separate resting, removal, or completion events. Accepted means
the command was accepted; it does not mean the order remains live. Trade payloads
give executed quantity; snapshots give remaining depth. A successful call returns
after all its state changes. Events contain no wall-clock timestamps.

## Queries and ownership

`best_bid()` / `best_ask()` return `optional<Level>` (empty for an empty side).
`depth(n)` returns up to n levels PER side, bids descending and asks ascending;
zero yields two empty lists. `Level` contains price, aggregate remaining quantity,
and order count. `live_order_count()` returns the total across both sides.
Query results and event vectors own their data and survive subsequent commands.
One thread owns the noncopyable, nonmovable book; calls are not synchronized.
An implementation pointer keeps future container choices out of the public API.

`validate_invariants()` is an explicit read-only diagnostic, returning normally
for consistent storage and throwing `std::logic_error` on a detected violation.
It checks strictly uncrossed best prices, nonempty levels, price/ID/quantity
bounds, the live-order limit, exact aggregate quantities, and a one-to-one
correspondence between owned orders and ID handles (including side, price, and
list position). It consumes no event sequence numbers. The scan costs expected
O(N + P) time with O(1) auxiliary space for N live orders and P price levels;
it is not automatically run by `process()`.

Engine tests call the checker at every command boundary, and storage tests
deliberately break individual invariants to verify detection. Iterator lifetime
and container integrity remain prerequisites for traversal and comparison;
this is a logical consistency check, not recovery from arbitrary memory
corruption. ASan/UBSan runs complement it with memory/undefined-behavior checks.
FIFO arrival semantics are verified separately through exact trade sequences.

## Worked examples (specification, not executable replay syntax)

Each example starts with an empty book and sequence 1. Abbreviated `A`, `T`, and
`R` below mean Accepted, Trade, and Rejected; prices are ticks.

Matching, FIFO, partial fill, and resting maker price:

```text
Add{1, sell, 100, 5} -> 1 A{add, 1}
Add{2, sell, 100, 4} -> 2 A{add, 2}
Add{3, buy, 101, 7} -> 3 A{add, 3}
                       4 T{maker=1, taker=3, price=100, quantity=5}
                       5 T{maker=2, taker=3, price=100, quantity=2}
Final: bids empty; asks [(100, quantity=2, orders=1)]; live ID 2.
```

Replacement loses priority, even for a size reduction; rejection preserves state:

```text
Add{10, buy, 99, 5} -> 1 A{add, 10}
Add{11, buy, 99, 5} -> 2 A{add, 11}
Replace{10, 99, 3}  -> 3 A{replace, 10}  # queue now 11, 10
Replace{11, 99, 0}  -> 4 R{replace, 11, invalid_quantity}
Add{12, sell, 99, 6}-> 5 A{add, 12}
                      6 T{maker=11, taker=12, price=99, quantity=5}
                      7 T{maker=10, taker=12, price=99, quantity=1}
Final: bids [(99, quantity=2, orders=1)]; asks empty; live ID 10.
```

A crossing replacement trades at the opposite resting order's price:

```text
Add{20, sell, 105, 2} -> 1 A{add, 20}
Add{21, buy, 100, 3}  -> 2 A{add, 21}
Replace{21, 106, 3}   -> 3 A{replace, 21}
                        4 T{maker=20, taker=21, price=105, quantity=2}
Cancel{21}           -> 5 A{cancel, 21}
Cancel{21}           -> 6 R{cancel, 21, unknown_id}
Final: both sides empty.
```

These examples become engine tests as the corresponding operations are implemented.
