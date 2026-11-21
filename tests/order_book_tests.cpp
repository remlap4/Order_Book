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

void accepted(OrderBook& book, Add add, Sequence sequence) {
    const auto events = book.process(add);
    require(events.size() == 1 && events[0].sequence == sequence, "ack sequence/count");
    const auto* ack = std::get_if<Accepted>(&events[0].payload);
    require(ack && ack->id == add.id && ack->kind == CommandKind::add, "ack payload");
}

void rejected(OrderBook& book, Add add, RejectReason reason, Sequence sequence) {
    const auto events = book.process(add);
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

void unsupported_operations() {
    OrderBook book;
    accepted(book, {1, Side::buy, 100, 2}, 1);
    accepted(book, {2, Side::sell, 102, 3}, 2);
    for (const Command& command : std::vector<Command>{
             Cancel{1}, Replace{1, 99, 1}}) {
        bool threw = false;
        try { (void)book.process(command); }
        catch (const std::logic_error&) { threw = true; }
        require(threw, "unimplemented operation must fail explicitly");
        require(book.live_order_count() == 2, "unsupported command mutated count");
        check_level(book.best_bid().value(), 100, 2, 1);
        check_level(book.best_ask().value(), 102, 3, 1);
    }
    accepted(book, {3, Side::buy, 101, 1}, 3); // guards consume no sequence or ID
}

void check_trade(const Event& event, Sequence sequence, OrderId maker,
                 OrderId taker, Price price, Quantity quantity) {
    const auto* trade = std::get_if<Trade>(&event.payload);
    require(event.sequence == sequence && trade && trade->maker_id == maker
            && trade->taker_id == taker && trade->price == price
            && trade->quantity == quantity, "trade sequence/payload");
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
            const auto first = book.process(Add{30, taker_side, limit, 2});
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
            const auto second = book.process(Add{30, taker_side, limit, 5});
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
            const auto final = book.process(Add{40, taker_side, limit, 2});
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

                const auto events = book.process(Add{30, taker_side, price(2), quantity});
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
                    const auto followup = book.process(Add{70, maker_side, price(2), 4});
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

void fifo_and_handles() {
    // Inspect internal storage because aggregate public queries intentionally hide
    // individual IDs. Trade tests independently verify execution FIFO.
    detail::BookStorage storage;
    for (const auto side : {Side::buy, Side::sell}) {
        const OrderId offset = side == Side::buy ? 0 : 100;
        for (const OrderId id : {9ULL, 2ULL, 7ULL}) {
            storage.insert({id + offset, side, 100, id});
        }
    }
    const auto saved = storage.by_id.at(9).order;
    for (OrderId id = 1'000; id < 3'000; ++id) {
        storage.insert({id, Side::buy, static_cast<Price>(id), 1});
    }
    // Force hash rehash and verify handles still point to the owned list nodes.
    storage.by_id.rehash(storage.by_id.bucket_count() * 2);
    require(saved->id == 9 && saved->quantity == 9, "handle lifetime");
    for (const auto side : {Side::buy, Side::sell}) {
        const OrderId offset = side == Side::buy ? 0 : 100;
        const auto& level = (side == Side::buy ? storage.bids : storage.asks).at(100);
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
        unsupported_operations();
        symmetric_matching();
        multi_level_sweeps();
        fifo_and_handles();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
