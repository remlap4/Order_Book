#include "order_book/order_book.hpp"
#include "book_storage.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace order_book;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Check every command boundary, including recoverable rejections.
// The production matching path pays no scan cost.
std::vector<Event> checked_process(OrderBook& book, const Command& command) {
    book.validate_invariants();
    std::vector<Event> events;
    try {
        events = book.process(command);
    } catch (const std::logic_error&) {
        book.validate_invariants();
        throw;
    }
    book.validate_invariants();
    return events;
}

void accepted(OrderBook& book, Add add, Sequence sequence) {
    const auto events = checked_process(book, add);
    require(events.size() == 1 && events[0].sequence == sequence, "ack sequence/count");
    const auto* ack = std::get_if<Accepted>(&events[0].payload);
    require(ack && ack->id == add.id && ack->kind == CommandKind::add, "ack payload");
}

void rejected(OrderBook& book, Add add, RejectReason reason, Sequence sequence) {
    const auto events = checked_process(book, add);
    require(events.size() == 1 && events[0].sequence == sequence, "rejection sequence/count");
    const auto* error = std::get_if<Rejected>(&events[0].payload);
    require(error && error->id == add.id && error->kind == CommandKind::add
            && error->reason == reason, "rejection payload");
}

void check_level(const Level& level, Price price, Quantity quantity, std::size_t count) {
    require(level.price == price && level.total_quantity == quantity
            && level.order_count == count, "price level contents");
}

void add_and_query() {
    OrderBook book;
    require(!book.best_bid() && !book.best_ask() && book.live_order_count() == 0,
            "new book must be empty");
    require(book.depth(10).bids.empty() && book.depth(10).asks.empty(), "empty depth");
    // Arrival price order differs from sorted order on both sides.
    accepted(book, {90, Side::buy, 98, 7}, 1);
    accepted(book, {10, Side::buy, 100, 3}, 2);
    accepted(book, {50, Side::buy, 99, 4}, 3);
    accepted(book, {20, Side::buy, 100, 8}, 4);
    accepted(book, {80, Side::sell, 104, 9}, 5);
    accepted(book, {30, Side::sell, 102, 6}, 6);
    accepted(book, {60, Side::sell, 103, 2}, 7);
    accepted(book, {40, Side::sell, 102, 5}, 8);
    const auto snapshot = book.depth(100);
    require(snapshot.bids.size() == 3 && snapshot.asks.size() == 3, "depth size");
    check_level(snapshot.bids[0], 100, 11, 2);
    check_level(snapshot.bids[1], 99, 4, 1);
    check_level(snapshot.bids[2], 98, 7, 1);
    check_level(snapshot.asks[0], 102, 11, 2);
    check_level(snapshot.asks[1], 103, 2, 1);
    check_level(snapshot.asks[2], 104, 9, 1);
    check_level(book.best_bid().value(), 100, 11, 2);
    check_level(book.best_ask().value(), 102, 11, 2);
    require(book.depth(1).bids.size() == 1 && book.depth(1).asks.size() == 1,
            "depth limit applies per side");
    require(book.depth(0).bids.empty() && book.depth(0).asks.empty(), "zero depth");
    accepted(book, {70, Side::buy, 100, 1}, 9);
    check_level(snapshot.bids[0], 100, 11, 2); // snapshot owns its previous values
    check_level(book.best_bid().value(), 100, 12, 3);
    require(book.live_order_count() == 9, "live order count");
}

void invalid_adds() {
    OrderBook book;
    accepted(book, {1, Side::buy, 100, max_quantity}, 1);
    rejected(book, {1, Side::buy, 99, 5}, RejectReason::duplicate_id, 2);
    rejected(book, {1, Side::sell, 101, 5}, RejectReason::duplicate_id, 3);
    rejected(book, {1, Side::buy, 0, 0}, RejectReason::invalid_price, 4);
    rejected(book, {2, static_cast<Side>(255), 101, 1}, RejectReason::invalid_side, 5);
    rejected(book, {0, Side::buy, 99, 1}, RejectReason::invalid_id, 6);
    rejected(book, {2, Side::buy, 99, 0}, RejectReason::invalid_quantity, 7);
    require(book.live_order_count() == 1 && !book.best_ask(), "reject preserves book");
    check_level(book.best_bid().value(), 100, max_quantity, 1);
    accepted(book, {2, Side::buy, 100, max_quantity}, 8);
    check_level(book.best_bid().value(), 100, 2 * max_quantity, 2);
}

void check_trade(const Event& event, Sequence sequence, OrderId maker,
                 OrderId taker, Price price, Quantity quantity) {
    const auto* trade = std::get_if<Trade>(&event.payload);
    require(event.sequence == sequence && trade && trade->maker_id == maker
            && trade->taker_id == taker && trade->price == price
            && trade->quantity == quantity, "trade sequence/payload");
}

void cancel_event(OrderBook& book, OrderId id, Sequence sequence,
                  std::optional<RejectReason> reason = std::nullopt) {
    const auto events = checked_process(book, Cancel{id});
    require(events.size() == 1 && events[0].sequence == sequence, "cancel event count/sequence");
    if (reason) {
        const auto* error = std::get_if<Rejected>(&events[0].payload);
        require(error && error->kind == CommandKind::cancel && error->id == id
                && error->reason == *reason, "cancel rejection payload");
    } else {
        const auto* ack = std::get_if<Accepted>(&events[0].payload);
        require(ack && ack->kind == CommandKind::cancel && ack->id == id,
                "cancel acknowledgement payload");
    }
}

void cancellation() {
    for (const auto side : {Side::buy, Side::sell}) {
        const auto opposite = side == Side::buy ? Side::sell : Side::buy;
        // Remove each queue position independently, then execute survivors to
        // verify cancellation preserves their FIFO priority and index handles.
        for (const OrderId removed : {90ULL, 10ULL, 50ULL}) {
            OrderBook book;
            accepted(book, {90, side, 100, 2}, 1);
            accepted(book, {10, side, 100, 3}, 2);
            accepted(book, {50, side, 100, 4}, 3);
            const auto saved = book.depth(1);
            cancel_event(book, 0, 4, RejectReason::invalid_id);
            cancel_event(book, 999, 5, RejectReason::unknown_id);
            check_level((side == Side::buy ? book.best_bid() : book.best_ask()).value(),
                        100, 9, 3);
            cancel_event(book, removed, 6);
            cancel_event(book, removed, 7, RejectReason::unknown_id);
            const Quantity canceled = removed == 90 ? 2 : (removed == 10 ? 3 : 4);
            check_level((side == Side::buy ? book.best_bid() : book.best_ask()).value(),
                        100, 9 - canceled, 2);
            require(book.live_order_count() == 2, "cancel updates live count");
            check_level((side == Side::buy ? saved.bids : saved.asks)[0], 100, 9, 3);
            // Reusing a canceled ID appends it after the surviving orders.
            accepted(book, {removed, side, 100, 1}, 8);
            const auto fills = checked_process(book, Add{70, opposite, 100, 10 - canceled});
            require(fills.size() == 4 && fills[0].sequence == 9, "post-cancel fill count");
            std::size_t index = 1;
            Sequence sequence = 10;
            for (const OrderId id : {90ULL, 10ULL, 50ULL}) {
                if (id != removed) {
                    check_trade(fills[index++], sequence++, id, 70, 100,
                                id == 90 ? 2 : (id == 10 ? 3 : 4));
                }
            }
            check_trade(fills[index], sequence, removed, 70, 100, 1);
            require(!book.best_bid() && !book.best_ask() && book.live_order_count() == 0,
                    "post-cancel fills empty book");
        }

        OrderBook book;
        const Price worse = side == Side::buy ? 99 : 101;
        const Price other = side == Side::buy ? 110 : 90;
        accepted(book, {1, side, 100, 5}, 1);
        accepted(book, {2, side, worse, 7}, 2);
        accepted(book, {3, opposite, other, 8}, 3);
        const auto partial = checked_process(book, Add{4, opposite, 100, 2});
        require(partial.size() == 2, "cancel setup partial fill");
        check_trade(partial[1], 5, 1, 4, 100, 2);
        cancel_event(book, 4, 6, RejectReason::unknown_id); // fully filled taker
        cancel_event(book, 1, 7); // remove only the remaining three units
        const auto snapshot = book.depth(1);
        check_level((side == Side::buy ? snapshot.bids : snapshot.asks)[0], worse, 7, 1);
        check_level((side == Side::buy ? snapshot.asks : snapshot.bids)[0], other, 8, 1);
        require(book.live_order_count() == 2, "cancel removes empty best level");
        cancel_event(book, 2, 8);
        require(!(side == Side::buy ? book.best_bid() : book.best_ask()), "empty canceled side");
        cancel_event(book, 3, 9);
        require(!book.best_bid() && !book.best_ask() && book.live_order_count() == 0,
                "last cancel empties book");
        for (const std::size_t limit : {0U, 1U, 100U}) {
            const auto empty = book.depth(limit);
            require(empty.bids.empty() && empty.asks.empty(), "empty depth after cancel");
        }
        cancel_event(book, 3, 10, RejectReason::unknown_id);
        cancel_event(book, 0, 11, RejectReason::invalid_id);
        accepted(book, {1, side, 100, 1}, 12); // queries consumed no sequence
    }
}

void replacement() {
    const auto check_ack = [](const Event& event, Sequence sequence, OrderId id) {
        const auto* ack = std::get_if<Accepted>(&event.payload);
        require(event.sequence == sequence && ack && ack->kind == CommandKind::replace
                && ack->id == id, "replace acknowledgement");
    };
    for (const auto side : {Side::buy, Side::sell}) {
        const auto opposite = side == Side::buy ? Side::sell : Side::buy;
        // Unchanged fields, reductions, and increases all move the head to tail.
        for (const Quantity size : {5ULL, 2ULL, 8ULL}) {
            OrderBook book;
            accepted(book, {90, side, 100, 5}, 1);
            accepted(book, {10, side, 100, 3}, 2);
            const auto events = checked_process(book, Replace{90, 100, size});
            require(events.size() == 1, "passive replace emits only acknowledgement");
            check_ack(events[0], 3, 90);
            check_level((side == Side::buy ? book.best_bid() : book.best_ask()).value(),
                        100, size + 3, 2);
            const auto fills = checked_process(book, Add{20, opposite, 100, size + 3});
            require(fills.size() == 3, "replace priority fills");
            check_trade(fills[1], 5, 10, 20, 100, 3);
            check_trade(fills[2], 6, 90, 20, 100, size);
            require(book.live_order_count() == 0, "replace full cleanup");
        }
        // A price change joins an existing queue and removes the old sole level.
        {
            OrderBook book;
            const Price old = side == Side::buy ? 99 : 101;
            accepted(book, {1, side, old, 7}, 1);
            accepted(book, {2, side, 100, 3}, 2);
            auto events = checked_process(book, Replace{1, 100, 2});
            check_ack(events[0], 3, 1);
            require(events.size() == 1 && book.live_order_count() == 2,
                    "price change preserves count");
            const auto depth = book.depth(10);
            require((side == Side::buy ? depth.bids : depth.asks).size() == 1,
                    "replace removes old level");
            const auto fills = checked_process(book, Add{3, opposite, 100, 5});
            require(fills.size() == 3, "price change FIFO fills");
            check_trade(fills[1], 5, 2, 3, 100, 3);
            check_trade(fills[2], 6, 1, 3, 100, 2);
        }
        // Replace the remaining size of a partially filled order. Sweep makers
        // at their prices, exercising partial fill, exact fill, and new residual.
        for (const Quantity size : {2ULL, 5ULL, 8ULL}) {
            OrderBook book;
            const Price first = side == Side::buy ? 102 : 98;
            const Price limit = side == Side::buy ? 103 : 97;
            accepted(book, {1, side, 100, 7}, 1);
            const auto partial = checked_process(book, Add{9, opposite, 100, 2});
            check_trade(partial[1], 3, 1, 9, 100, 2);
            accepted(book, {90, opposite, first, 3}, 4);
            accepted(book, {10, opposite, limit, 2}, 5);
            const auto events = checked_process(book, Replace{1, limit, size});
            check_ack(events[0], 6, 1);
            require(events.size() == (size == 2 ? 2U : 3U), "crossing replace event count");
            check_trade(events[1], 7, 90, 1, first, size == 2 ? 2 : 3);
            if (size != 2) check_trade(events[2], 8, 10, 1, limit, 2);
            const auto depth = book.depth(10);
            const auto& own = side == Side::buy ? depth.bids : depth.asks;
            const auto& other = side == Side::buy ? depth.asks : depth.bids;
            require(own.size() == (size == 8 ? 1U : 0U)
                    && other.size() == (size == 2 ? 2U : 0U), "crossing replace final levels");
            if (size == 2) {
                check_level(other[0], first, 1, 1);
                check_level(other[1], limit, 2, 1);
            }
            if (size == 8) {
                check_level(own[0], limit, 3, 1);
                cancel_event(book, 1, 9); // retained ID handle points to the residual
            } else {
                cancel_event(book, 1, size == 2 ? 8 : 9, RejectReason::unknown_id);
            }
        }
        {
            OrderBook book;
            accepted(book, {1, side, 100, 5}, 1);
            accepted(book, {2, side, 100, 3}, 2);
            const Price crossing = side == Side::buy ? 110 : 90;
            accepted(book, {3, opposite, crossing, 4}, 3);
            Sequence sequence = 4;
            const auto reject = [&](Replace command, RejectReason reason) {
                const auto events = checked_process(book, command);
                require(events.size() == 1 && events[0].sequence == sequence++, "replace rejection sequence");
                const auto* error = std::get_if<Rejected>(&events[0].payload);
                require(error && error->kind == CommandKind::replace && error->id == command.id
                        && error->reason == reason, "replace rejection payload");
                const auto depth = book.depth(10);
                require(depth.bids.size() == 1 && depth.asks.size() == 1
                        && book.live_order_count() == 3, "rejected replace preserves levels/count");
                check_level((side == Side::buy ? depth.bids : depth.asks)[0], 100, 8, 2);
                check_level((side == Side::buy ? depth.asks : depth.bids)[0], crossing, 4, 1);
            };
            reject({0, 0, 0}, RejectReason::invalid_id);
            reject({1, 0, 0}, RejectReason::invalid_price);
            reject({1, max_price + 1, 1}, RejectReason::invalid_price);
            reject({1, crossing, 0}, RejectReason::invalid_quantity);
            reject({1, crossing, max_quantity + 1}, RejectReason::invalid_quantity);
            reject({999, 0, 1}, RejectReason::invalid_price);
            reject({999, crossing, 1}, RejectReason::unknown_id);
            // Execution reveals individual remaining sizes and unchanged FIFO,
            // which aggregate snapshots alone cannot prove.
            const auto fills = checked_process(book, Add{4, opposite, 100, 8});
            require(fills.size() == 3, "rejected replace preserves FIFO count");
            check_trade(fills[1], sequence + 1, 1, 4, 100, 5);
            check_trade(fills[2], sequence + 2, 2, 4, 100, 3);
        }
    }
}

void symmetric_matching() {
    // Mirror each scenario and exercise both equality and strictly crossing
    // limits. Nonmonotonic IDs distinguish FIFO arrival from numeric ID order.
    for (const auto maker_side : {Side::buy, Side::sell}) {
        for (const bool equal_limit : {false, true}) {
            OrderBook book;
            const auto taker_side = maker_side == Side::buy ? Side::sell : Side::buy;
            const Price limit = equal_limit ? 100 : (taker_side == Side::buy ? 101 : 99);
            accepted(book, {90, maker_side, 100, 5}, 1);
            accepted(book, {10, maker_side, 100, 4}, 2);

            // Rejected crossing commands must not fill or disturb priority.
            rejected(book, {90, taker_side, limit, 7}, RejectReason::duplicate_id, 3);
            rejected(book, {30, taker_side, limit, 0}, RejectReason::invalid_quantity, 4);
            const auto first = checked_process(book, Add{30, taker_side, limit, 2});
            require(first.size() == 2 && first[0].sequence == 5, "partial fill events");
            const auto* ack = std::get_if<Accepted>(&first[0].payload);
            require(ack && ack->kind == CommandKind::add && ack->id == 30,
                    "acceptance precedes trades");
            check_trade(first[1], 6, 90, 30, 100, 2);
            check_level((maker_side == Side::buy ? book.best_bid() : book.best_ask()).value(),
                        100, 7, 2);
            require(book.live_order_count() == 2, "fully executed taker must not rest");
            require(!(taker_side == Side::buy ? book.best_bid() : book.best_ask()),
                    "taker side remains empty");

            // Reusing the filled taker's ID is allowed. The partially filled
            // maker must still execute before the second arrival.
            const auto second = checked_process(book, Add{30, taker_side, limit, 5});
            require(second.size() == 3 && second[0].sequence == 7, "FIFO event count");
            const auto* second_ack = std::get_if<Accepted>(&second[0].payload);
            require(second_ack && second_ack->id == 30
                    && second_ack->kind == CommandKind::add, "FIFO acknowledgement");
            check_trade(second[1], 8, 90, 30, 100, 3);
            check_trade(second[2], 9, 10, 30, 100, 2);
            check_level((maker_side == Side::buy ? book.best_bid() : book.best_ask()).value(),
                        100, 2, 1);
            require(book.live_order_count() == 1, "full maker removal");
            // Nine submitted maker units = seven traded + two remaining.
            const auto final = checked_process(book, Add{40, taker_side, limit, 2});
            require(final.size() == 2 && final[0].sequence == 10, "exact fill events");
            check_trade(final[1], 11, 10, 40, 100, 2);
            require(!book.best_bid() && !book.best_ask() && book.live_order_count() == 0,
                    "exact fill removes final order and level");
            accepted(book, {90, maker_side, 100, 1}, 12);
            check_level((maker_side == Side::buy ? book.best_bid() : book.best_ask()).value(),
                        100, 1, 1);
            check_trade(first[1], 6, 90, 30, 100, 2); // returned events own their data
        }
    }
}

void multi_level_sweeps() {
    // Mirrored prices exercise ascending asks and descending bids. Arrival order
    // deliberately differs from price order, and IDs differ from FIFO order.
    for (const auto maker_side : {Side::buy, Side::sell}) {
        const auto taker_side = maker_side == Side::buy ? Side::sell : Side::buy;
        const Price direction = maker_side == Side::sell ? 1 : -1;
        const auto price = [direction](Price step) { return 100 + direction * step; };
        for (const bool beyond_limit : {false, true}) {
            for (const Quantity quantity : {8ULL, 14ULL, 17ULL}) {
                OrderBook book;
                Sequence sequence = 1;
                accepted(book, {40, maker_side, price(2), 5}, sequence++);
                accepted(book, {90, maker_side, price(0), 2}, sequence++);
                if (beyond_limit) {
                    accepted(book, {60, maker_side, price(3), 6}, sequence++);
                }
                accepted(book, {20, maker_side, price(1), 4}, sequence++);
                accepted(book, {10, maker_side, price(0), 3}, sequence++);

                const auto events = checked_process(book, Add{30, taker_side, price(2), quantity});
                const bool partial = quantity == 8;
                require(events.size() == (partial ? 4U : 5U), "sweep event count");
                const auto* ack = std::get_if<Accepted>(&events[0].payload);
                require(events[0].sequence == sequence++ && ack
                        && ack->kind == CommandKind::add && ack->id == 30,
                        "sweep acknowledgement precedes trades");
                check_trade(events[1], sequence++, 90, 30, price(0), 2);
                check_trade(events[2], sequence++, 10, 30, price(0), 3);
                check_trade(events[3], sequence++, 20, 30, price(1), partial ? 3 : 4);
                if (!partial) check_trade(events[4], sequence++, 40, 30, price(2), 5);

                // Assert the entire public state, including removed levels and
                // untouched liquidity beyond the limit. Both sides conserve size.
                const Quantity residual = quantity > 14 ? quantity - 14 : 0;
                const auto snapshot = book.depth(100);
                const auto& makers = maker_side == Side::buy ? snapshot.bids : snapshot.asks;
                const auto& takers = taker_side == Side::buy ? snapshot.bids : snapshot.asks;
                require(makers.size() == (partial ? 2U : 0U) + (beyond_limit ? 1U : 0U),
                        "sweep removes exhausted levels only");
                if (partial) {
                    check_level(makers[0], price(1), 1, 1);
                    check_level(makers[1], price(2), 5, 1);
                }
                if (beyond_limit) check_level(makers.back(), price(3), 6, 1);
                require(takers.size() == (residual ? 1U : 0U), "sweep residual level count");
                if (residual) check_level(takers[0], price(2), residual, 1);
                require(book.live_order_count() == makers.size() + takers.size(),
                        "sweep ID index count");
                const auto best_maker = maker_side == Side::buy ? book.best_bid() : book.best_ask();
                const auto best_taker = taker_side == Side::buy ? book.best_bid() : book.best_ask();
                require(best_maker.has_value() == !makers.empty()
                        && best_taker.has_value() == !takers.empty(), "sweep empty best prices");
                if (best_maker) check_level(*best_maker, makers[0].price,
                                           makers[0].total_quantity, makers[0].order_count);
                if (best_taker) check_level(*best_taker, price(2), residual, 1);
                Quantity traded = 0;
                for (std::size_t i = 1; i < events.size(); ++i) {
                    traded += std::get<Trade>(events[i].payload).quantity;
                }
                Quantity resting = 0;
                for (const auto& level : makers) resting += level.total_quantity;
                require(traded + residual == quantity
                        && traded + resting == 14 + (beyond_limit ? 6U : 0U),
                        "sweep quantity conservation");

                if (residual) {
                    // A crossing residual creates its own level: earlier orders
                    // at that price would already have crossed the makers. A later
                    // arrival must queue behind it, verified through actual fills.
                    accepted(book, {5, taker_side, price(2), 2}, sequence++);
                    const auto followup = checked_process(book, Add{70, maker_side, price(2), 4});
                    require(followup.size() == 3, "residual FIFO event count");
                    const auto* followup_ack = std::get_if<Accepted>(&followup[0].payload);
                    require(followup[0].sequence == sequence++ && followup_ack
                            && followup_ack->kind == CommandKind::add && followup_ack->id == 70,
                            "residual FIFO acknowledgement");
                    check_trade(followup[1], sequence++, 30, 70, price(2), 3);
                    check_trade(followup[2], sequence++, 5, 70, price(2), 1);
                    check_level((taker_side == Side::buy ? book.best_bid() : book.best_ask()).value(),
                                price(2), 1, 1);
                    require(book.live_order_count() == (beyond_limit ? 2U : 1U),
                            "residual full-fill index cleanup");
                }
                // Filled maker and taker IDs must be reusable, even after several
                // level deletions. A passive price avoids changing the other side.
                accepted(book, {90, taker_side, price(-2), 1}, sequence++);
                accepted(book, {30, taker_side, price(-2), 1}, sequence++);
                check_level((taker_side == Side::buy ? book.depth(100).bids
                                                   : book.depth(100).asks).back(),
                            price(-2), 2, 2);
            }
        }
    }
}

void invariant_diagnostics() {
    detail::BookStorage empty;
    empty.validate_invariants();
    OrderBook empty_book;
    empty_book.validate_invariants();

    // Each fixture owns valid nodes throughout the test. Corrupt logical state
    // independently so a different invariant cannot hide a missed defect.
    const auto detects = [](const auto& corrupt) {
        detail::BookStorage storage;
        storage.insert({1, Side::buy, 99, 2});
        storage.insert({2, Side::buy, 99, 3});
        storage.insert({3, Side::sell, 101, 4});
        storage.validate_invariants();
        corrupt(storage);
        bool threw = false;
        try { storage.validate_invariants(); }
        catch (const std::logic_error&) { threw = true; }
        require(threw, "checker must detect inconsistent storage");
    };
    detects([](auto& s) { s.insert({4, Side::buy, 101, 1}); });
    detects([](auto& s) { s.insert({4, Side::sell, 98, 1}); });
    detects([](auto& s) { s.bids.try_emplace(98); });
    detects([](auto& s) { s.insert({4, Side::buy, 0, 1}); });
    detects([](auto& s) { s.insert({4, Side::sell, max_price + 1, 1}); });
    detects([](auto& s) { s.bids.at(99).orders.front().quantity = 0; });
    detects([](auto& s) { s.bids.at(99).orders.front().quantity = max_quantity + 1; });
    detects([](auto& s) { s.bids.at(99).orders.front().id = 0; });
    detects([](auto& s) { ++s.asks.at(101).total_quantity; });
    detects([](auto& s) { s.by_id.erase(1); });
    detects([](auto& s) { s.by_id.emplace(4, s.by_id.at(1)); });
    detects([](auto& s) { s.by_id.at(1).side = Side::sell; });
    detects([](auto& s) { s.by_id.at(1).side = static_cast<Side>(255); });
    detects([](auto& s) { s.by_id.at(1).price = 98; });
    detects([](auto& s) { s.by_id.at(1).order = s.by_id.at(2).order; });
    detects([](auto& s) { s.bids.at(99).orders.back().id = 1; });
}

void fifo_and_handles() {
    // Inspect internal storage because aggregate public queries intentionally hide
    // individual IDs. Trade tests independently verify execution FIFO.
    detail::BookStorage storage;
    for (const auto side : {Side::buy, Side::sell}) {
        const OrderId offset = side == Side::buy ? 0 : 100;
        for (const OrderId id : {9ULL, 2ULL, 7ULL}) {
            storage.insert({id + offset, side, side == Side::buy ? 100 : 4000, id});
        }
    }
    const auto saved = storage.by_id.at(9).order;
    for (OrderId id = 1'000; id < 3'000; ++id) {
        storage.insert({id, Side::buy, static_cast<Price>(id), 1});
    }
    // Force hash rehash and verify handles still point to the owned list nodes.
    storage.by_id.rehash(storage.by_id.bucket_count() * 2);
    storage.validate_invariants();
    require(saved->id == 9 && saved->quantity == 9, "handle lifetime");
    for (const auto side : {Side::buy, Side::sell}) {
        const OrderId offset = side == Side::buy ? 0 : 100;
        const auto& level = (side == Side::buy ? storage.bids : storage.asks).at(side == Side::buy ? 100 : 4000);
        std::vector<OrderId> ids;
        for (const auto& order : level.orders) ids.push_back(order.id);
        require(ids == std::vector<OrderId>{9 + offset, 2 + offset, 7 + offset},
                "FIFO must follow arrival, not numeric ID");
        require(level.total_quantity == 18, "FIFO level aggregate");
    }
    for (const auto& [id, handle] : storage.by_id) {
        require(handle.order->id == id, "ID handle identity");
        const auto& levels = handle.side == Side::buy ? storage.bids : storage.asks;
        require(levels.contains(handle.price), "ID handle price membership");
    }
}
} // namespace

int main() {
    try {
        add_and_query();
        invalid_adds();
        replacement();
        cancellation();
        symmetric_matching();
        multi_level_sweeps();
        fifo_and_handles();
        invariant_diagnostics();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
