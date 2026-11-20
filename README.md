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
- Order-ID lookup, best bid/ask, and depth snapshots

Tests cover matching at one price level on both sides, FIFO priority, aggregate
quantities, rejection behavior, and storage lifetime. Multi-level sweep and
residual-resting edge cases still need dedicated verification.

Cancellation, replacement, replay, and benchmarking are not yet implemented.
No performance results are claimed yet.

## Design

The baseline uses ordered price maps, FIFO lists at each price level, and
a hash index for order lookup. Prices and quantities use integer types,
and one thread owns all mutable book state. Normal and sanitizer builds
verify the implementation.

## Documentation

- [Build and run instructions](docs/instructions.md)
- [Engine contract](docs/contract.md)
