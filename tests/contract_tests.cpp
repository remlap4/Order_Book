#include "order_book/order_book.hpp"

#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

using namespace order_book;

// Check the public API signatures independently of runtime behavior tests.
static_assert(std::is_same_v<decltype(std::declval<OrderBook&>().process(
    std::declval<const Command&>())), std::vector<Event>>);
static_assert(std::is_same_v<decltype(std::declval<const OrderBook&>().best_bid()),
    std::optional<Level>>);
static_assert(std::is_same_v<decltype(std::declval<const OrderBook&>().best_ask()),
    std::optional<Level>>);
static_assert(std::is_same_v<decltype(std::declval<const OrderBook&>().depth(0)),
    DepthSnapshot>);
static_assert(noexcept(std::declval<const OrderBook&>().live_order_count()));
static_assert(!std::is_copy_constructible_v<OrderBook>);
static_assert(max_quantity <= std::numeric_limits<Quantity>::max() / max_live_orders);

int main() {
    // Explicit checks stay active in Release builds, unlike the assert macro.
    int failures = 0;
    const auto check = [&](const Command& command,
                           std::optional<RejectReason> expected, const char* name) {
        if (validate_fields(command) != expected) {
            std::cerr << "FAIL: " << name << '\n';
            ++failures;
        }
    };

    check(Add{1, Side::buy, 1, 1}, std::nullopt, "minimum buy fields");
    check(Add{std::numeric_limits<OrderId>::max(), Side::sell, max_price, max_quantity},
          std::nullopt, "maximum sell fields");
    check(Cancel{1}, std::nullopt, "cancel fields do not imply ID exists");
    check(Replace{1, max_price, max_quantity}, std::nullopt, "replace bounds");
    check(Add{0, static_cast<Side>(255), 0, 0}, RejectReason::invalid_id, "ID first");
    check(Add{1, static_cast<Side>(255), 0, 0}, RejectReason::invalid_side, "side second");
    check(Add{1, Side::buy, 0, 0}, RejectReason::invalid_price, "price before quantity");
    check(Add{1, Side::buy, -1, 1}, RejectReason::invalid_price, "negative price");
    check(Add{1, Side::buy, max_price + 1, 1}, RejectReason::invalid_price, "price cap");
    check(Add{1, Side::sell, 1, 0}, RejectReason::invalid_quantity, "zero quantity");
    check(Add{1, Side::sell, 1, max_quantity + 1}, RejectReason::invalid_quantity,
          "quantity cap");
    check(Cancel{0}, RejectReason::invalid_id, "zero cancel ID");
    check(Replace{0, 0, 0}, RejectReason::invalid_id, "replace ID precedence");
    check(Replace{1, 0, 0}, RejectReason::invalid_price, "replace price precedence");
    check(Replace{1, max_price + 1, 1}, RejectReason::invalid_price, "replace price cap");
    check(Replace{1, 1, 0}, RejectReason::invalid_quantity, "replace zero quantity");
    check(Replace{1, 1, max_quantity + 1}, RejectReason::invalid_quantity,
          "replace quantity cap");
    return failures == 0 ? 0 : 1;
}
