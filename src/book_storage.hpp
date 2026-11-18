#pragma once

#include "order_book/types.hpp"

#include <iterator>
#include <list>
#include <map>
#include <unordered_map>

namespace order_book::detail {

struct RestingOrder {
    OrderId id;
    Quantity quantity;
};

struct PriceLevel {
    Quantity total_quantity = 0;
    std::list<RestingOrder> orders;
};

struct OrderHandle {
    Side side;
    Price price;
    std::list<RestingOrder>::iterator order;
};

// Internal storage shared with storage tests, not part of the public API.
// Both maps ascend; bids are traversed in reverse for best-price priority.
// Lists own orders. Their iterators survive map insertion and hash rehashing.
// Future removal must erase the ID handle before its list node/empty price level.
struct BookStorage {
    std::map<Price, PriceLevel> bids;
    std::map<Price, PriceLevel> asks;
    std::unordered_map<OrderId, OrderHandle> by_id;

    // Only the engine calls this after validating fields, uniqueness and capacity.
    // Appending preserves arrival order even when IDs are not monotonically ordered.
    void insert(const Add& add) {
        auto& levels = add.side == Side::buy ? bids : asks;
        auto& level = levels.try_emplace(add.price).first->second;
        level.orders.push_back({add.id, add.quantity});
        by_id.emplace(add.id, OrderHandle{add.side, add.price, std::prev(level.orders.end())});
        level.total_quantity += add.quantity;
    }
};

} // namespace order_book::detail
