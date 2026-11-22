#pragma once

#include "order_book/types.hpp"

#include <memory>

namespace order_book {

// Supports limit adds with price/FIFO matching, cancellation, and read-only queries.
// One thread owns the book. There is no internal synchronization or reentrancy.
class OrderBook {
public:
    OrderBook();
    ~OrderBook();
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    // Process synchronously and return owned events. Logical rejection preserves
    // orders but advances the event sequence. Replace
    // currently throws std::logic_error without mutation. See docs/contract.md.
    [[nodiscard]] std::vector<Event> process(const Command& command);

    [[nodiscard]] std::optional<Level> best_bid() const;
    [[nodiscard]] std::optional<Level> best_ask() const;
    // Limit applies independently to each side; zero returns two empty vectors.
    [[nodiscard]] DepthSnapshot depth(std::size_t levels_per_side) const;
    [[nodiscard]] std::size_t live_order_count() const noexcept;

private:
    // Hide container choices so baseline and optimized storage share one API.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace order_book
