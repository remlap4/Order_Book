# Order Book

A C++20 limit order book and matching engine focused on correctness,
predictable behavior, and measurable performance.

The project explores the core mechanics of exchange-style order matching:
price-time priority, order lifecycle management, and efficient in-memory
storage. Performance improvements will be evaluated against a tested baseline
using reproducible workloads.

## Scope

- Single-instrument, single-threaded matching engine
- Limit orders with price-time priority
- Order cancellation and replacement
- Partial fills and matching across price levels
- Deterministic replay and correctness testing
- Latency and throughput benchmarks

Networking, live trading, and a graphical interface are outside the core scope.

## Current Progress

Implemented:

- Input validation and duplicate-order rejection
- Buy/sell matching with maker-price execution and partial fills
- FIFO storage within ordered price levels
- Cancellation by ID with remaining-quantity and empty-level cleanup
- Order-ID lookup, best bid/ask, and depth snapshots
- Explicit read-only storage invariant validation

Tests cover matching and multi-level sweeps on both sides, exact trade ordering,
limit-price stopping, full-fill cleanup, residual resting and FIFO priority,
quantity conservation, cancellation at every queue position, empty-book queries,
rejection behavior, and storage lifetime. Engine tests check invariants at every
command boundary; deliberate storage inconsistencies test the checker itself.

Replacement, replay, and benchmarking are not yet implemented.
No performance results are claimed yet.

## Design

The baseline uses ordered price maps, FIFO lists at each price level, and
a hash index for order lookup. Prices and quantities use integer types,
and one thread owns all mutable book state. Normal and sanitizer builds
verify the implementation.

Lists own resting orders; the ID index holds stable list iterators. Insertions
and index rehashing preserve these handles. Removal erases the handle before
destroying its order and any empty level. `validate_invariants()` checks that
prices are uncrossed, levels are nonempty, quantities and totals are valid, and
each order has exactly one matching ID handle. This explicit diagnostic scan
stays outside the normal matching path. See the contract for complexity and
failure behavior.

## Documentation

- [Build and run instructions](docs/instructions.md)
- [Engine contract](docs/contract.md)
