#include "order_book/order_book.hpp"
#include "book_storage.hpp"

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
    // Until later roadmap steps implement these operations, fail explicitly rather
    // than emit a misleading business rejection or silently leave a crossed book.
    const auto* add = std::get_if<Add>(&command);
    if (!add) {
        throw std::logic_error("Cancel and Replace are not implemented yet");
    }

    // Reserve sequence space conservatively for future matching (ack + all makers).
    // Leaving the final value unused also keeps next_sequence increment safe.
    constexpr auto budget = static_cast<Sequence>(max_live_orders) + 1;
    if (impl_->next_sequence > std::numeric_limits<Sequence>::max() - budget) {
        throw std::overflow_error("Event sequence exhausted");
    }
    auto& storage = impl_->storage;
    auto error = validate_fields(command);
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

    const bool crosses = add->side == Side::buy
        ? (!storage.asks.empty() && add->price >= storage.asks.begin()->first)
        : (!storage.bids.empty() && add->price <= storage.bids.rbegin()->first);
    if (crosses) {
        throw std::logic_error("Crossing adds require matching, not implemented yet");
    }

    // Allocate the acknowledgement before mutation. Storage allocation failures
    // remain terminal for this book as documented; no rollback is promised.
    std::vector<Event> events{{impl_->next_sequence, Accepted{CommandKind::add, add->id}}};
    storage.insert(*add);
    ++impl_->next_sequence;
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
