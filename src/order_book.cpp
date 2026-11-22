#include "order_book/order_book.hpp"
#include "book_storage.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace order_book {

struct OrderBook::Impl {
    detail::BookStorage storage;
    Sequence next_sequence = 1;
};

namespace {
Level summarize(Price price, const detail::PriceLevel& level) {
    return {price, level.total_quantity, level.orders.size()};
}
} // namespace

OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}
OrderBook::~OrderBook() = default;

std::vector<Event> OrderBook::process(const Command& command) {
    // Unsupported replacement must not consume sequence space or mutate orders.
    if (std::holds_alternative<Replace>(command)) {
        throw std::logic_error("Replace is not implemented yet");
    }

    // Reserve sequence space conservatively for matching (ack + all makers).
    // Leaving the final value unused also keeps next_sequence increment safe.
    constexpr auto budget = static_cast<Sequence>(max_live_orders) + 1;
    if (impl_->next_sequence > std::numeric_limits<Sequence>::max() - budget) {
        throw std::overflow_error("Event sequence exhausted");
    }
    auto& storage = impl_->storage;
    auto error = validate_fields(command);
    if (const auto* cancel = std::get_if<Cancel>(&command)) {
        const auto found = storage.by_id.find(cancel->id);
        if (!error && found == storage.by_id.end()) error = RejectReason::unknown_id;
        if (error) {
            std::vector<Event> events{{impl_->next_sequence,
                Rejected{CommandKind::cancel, cancel->id, *error}}};
            ++impl_->next_sequence;
            return events;
        }

        // Allocate the sole event before mutation. Copy the handle so the index
        // can be erased before its owned list node; other FIFO positions survive.
        std::vector<Event> events{{impl_->next_sequence,
            Accepted{CommandKind::cancel, cancel->id}}};
        const auto handle = found->second;
        auto& levels = handle.side == Side::buy ? storage.bids : storage.asks;
        const auto level_it = levels.find(handle.price);
        auto& level = level_it->second;
        level.total_quantity -= handle.order->quantity;
        storage.by_id.erase(found);
        level.orders.erase(handle.order);
        if (level.orders.empty()) levels.erase(level_it);
        ++impl_->next_sequence;
        return events;
    }

    const auto* add = std::get_if<Add>(&command);
    if (!error && storage.by_id.contains(add->id)) {
        error = RejectReason::duplicate_id;
    }
    if (!error && storage.by_id.size() >= max_live_orders) {
        error = RejectReason::capacity_exceeded;
    }
    if (error) {
        std::vector<Event> events{{impl_->next_sequence,
            Rejected{CommandKind::add, add->id, *error}}};
        ++impl_->next_sequence;
        return events;
    }

    // Allocate the acknowledgement before mutation. Storage allocation failures
    // remain terminal for this book as documented; no rollback is promised.
    std::vector<Event> events{{impl_->next_sequence, Accepted{CommandKind::add, add->id}}};
    ++impl_->next_sequence;

    // Both sides share the fill logic; only best-price selection and crossing
    // direction differ. The incoming order is not indexed until it rests.
    auto remaining = add->quantity;
    auto& opposite = add->side == Side::buy ? storage.asks : storage.bids;
    while (remaining != 0 && !opposite.empty()) {
        const auto level_it = add->side == Side::buy
            ? opposite.begin() : std::prev(opposite.end());
        const auto price = level_it->first;
        if (add->side == Side::buy ? add->price < price : add->price > price) break;

        auto& level = level_it->second;
        auto& maker = level.orders.front();
        const auto filled = std::min(remaining, maker.quantity);
        events.push_back({impl_->next_sequence, Trade{maker.id, add->id, price, filled}});
        ++impl_->next_sequence;
        remaining -= filled;
        maker.quantity -= filled;
        level.total_quantity -= filled;

        // A partially filled maker keeps its FIFO position. On full fill, erase
        // its handle before destroying the node, then remove any empty level.
        if (maker.quantity == 0) {
            storage.by_id.erase(maker.id);
            level.orders.pop_front();
            if (level.orders.empty()) opposite.erase(level_it);
        }
    }
    if (remaining != 0) {
        storage.insert({add->id, add->side, add->price, remaining});
    }
    return events;
}

std::optional<Level> OrderBook::best_bid() const {
    const auto& bids = impl_->storage.bids;
    if (bids.empty()) return std::nullopt;
    return summarize(bids.rbegin()->first, bids.rbegin()->second);
}

std::optional<Level> OrderBook::best_ask() const {
    const auto& asks = impl_->storage.asks;
    if (asks.empty()) return std::nullopt;
    return summarize(asks.begin()->first, asks.begin()->second);
}

DepthSnapshot OrderBook::depth(std::size_t levels_per_side) const {
    // Copy only the requested levels; snapshots cannot expose internal iterators.
    DepthSnapshot result;
    const auto& storage = impl_->storage;
    for (auto it = storage.bids.rbegin(); it != storage.bids.rend()
         && result.bids.size() < levels_per_side; ++it) {
        result.bids.push_back(summarize(it->first, it->second));
    }
    for (auto it = storage.asks.begin(); it != storage.asks.end()
         && result.asks.size() < levels_per_side; ++it) {
        result.asks.push_back(summarize(it->first, it->second));
    }
    return result;
}

std::size_t OrderBook::live_order_count() const noexcept {
    return impl_->storage.by_id.size();
}

} // namespace order_book
