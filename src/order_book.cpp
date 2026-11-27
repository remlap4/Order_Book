#include "order_book/order_book.hpp"
#include "order_book_impl.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace order_book {

namespace {
Level summarize(Price price, const detail::PriceLevel& level) {
    return {price, level.total_quantity, level.orders.size()};
}
} // namespace

OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}
OrderBook::~OrderBook() = default;

std::vector<Event> OrderBook::process(const Command& command) {
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
        storage.erase(found);
        ++impl_->next_sequence;
        return events;
    }

    // Normalize a validated replacement into an incoming limit order. Its old
    // slot is reused, so capacity/duplicate checks apply only to new adds.
    const auto* replacement = std::get_if<Replace>(&command);
    const auto kind = replacement ? CommandKind::replace : CommandKind::add;
    Add incoming{};
    auto original = storage.by_id.end();
    if (replacement) {
        incoming.id = replacement->id;
        original = storage.by_id.find(replacement->id);
        if (!error && original == storage.by_id.end()) error = RejectReason::unknown_id;
        if (!error) {
            incoming = {replacement->id, original->second.side,
                        replacement->price, replacement->quantity};
        }
    } else {
        incoming = std::get<Add>(command);
        if (!error && storage.by_id.contains(incoming.id)) error = RejectReason::duplicate_id;
        if (!error && storage.by_id.size() >= max_live_orders) error = RejectReason::capacity_exceeded;
    }
    const auto* add = &incoming;
    if (error) {
        std::vector<Event> events{{impl_->next_sequence,
            Rejected{kind, add->id, *error}}};
        ++impl_->next_sequence;
        return events;
    }

    // Allocate the acknowledgement before mutation. Storage allocation failures
    // remain terminal for this book as documented; no rollback is promised.
    std::vector<Event> events{{impl_->next_sequence, Accepted{kind, add->id}}};
    ++impl_->next_sequence;
    // All recoverable checks and acknowledgement allocation precede removal.
    // Unconditional unlink/reinsert resets priority even for unchanged fields.
    // Count and quantity bounds still cap aggregates at 10^18 after replacement.
    if (replacement) storage.erase(original);

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

void OrderBook::validate_invariants() const {
    impl_->storage.validate_invariants();
}

} // namespace order_book
