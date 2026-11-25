#pragma once

#include "order_book/types.hpp"

#include <iterator>
#include <list>
#include <map>
#include <stdexcept>
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
// Removal must erase the ID handle before its list node/empty price level.
struct BookStorage {
    std::map<Price, PriceLevel> bids;
    std::map<Price, PriceLevel> asks;
    std::unordered_map<OrderId, OrderHandle> by_id;

    // Walk owned nodes, never dereference index handles. Each node must map back
    // to itself; together with equal counts this proves a one-to-one index and
    // rejects duplicate IDs. Like container traversal, iterator comparison assumes
    // valid iterator lifetimes; sanitizers complement this logical consistency check.
    void validate_invariants() const {
        if (!bids.empty() && !asks.empty() && bids.rbegin()->first >= asks.begin()->first) {
            throw std::logic_error("Crossed resting book");
        }
        std::size_t count = 0;
        const auto check_side = [&](const auto& levels, Side side) {
            for (const auto& [price, level] : levels) {
                if (price <= 0 || price > max_price || level.orders.empty()) {
                    throw std::logic_error("Invalid or empty price level");
                }
                Quantity total = 0;
                for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
                    if (it->id == 0 || it->quantity == 0 || it->quantity > max_quantity) {
                        throw std::logic_error("Invalid resting order");
                    }
                    if (count == max_live_orders) {
                        throw std::logic_error("Live order limit exceeded");
                    }
                    ++count;
                    // Checked count and per-order bounds keep this sum <= 10^18.
                    total += it->quantity;
                    const auto found = by_id.find(it->id);
                    if (found == by_id.end() || found->second.side != side
                        || found->second.price != price || found->second.order != it) {
                        throw std::logic_error("Order index mismatch");
                    }
                }
                if (total != level.total_quantity) {
                    throw std::logic_error("Level aggregate mismatch");
                }
            }
        };
        check_side(bids, Side::buy);
        check_side(asks, Side::sell);
        if (count != by_id.size()) throw std::logic_error("Order index count mismatch");
    }

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
