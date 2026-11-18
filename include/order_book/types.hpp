#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace order_book {

using OrderId = std::uint64_t;
using Quantity = std::uint64_t;
using Price = std::int64_t;
using Sequence = std::uint64_t;

// Fixed limits bound total resting quantity to 10^18, safely below uint64_t max.
// Prices are integer ticks; conversion from currency belongs outside the engine.
inline constexpr Price max_price = 1'000'000'000'000;
inline constexpr Quantity max_quantity = 1'000'000'000'000;
inline constexpr std::size_t max_live_orders = 1'000'000;

enum class Side : std::uint8_t { buy, sell };
enum class CommandKind : std::uint8_t { add, cancel, replace };
enum class RejectReason : std::uint8_t {
    invalid_id,
    invalid_side,
    invalid_price,
    invalid_quantity,
    duplicate_id,
    unknown_id,
    capacity_exceeded
};

struct Add {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
};

struct Cancel {
    OrderId id;
};

// Quantity is the NEW remaining quantity, not a delta or original total size.
// The existing order supplies the side; even an unchanged replace loses priority.
struct Replace {
    OrderId id;
    Price price;
    Quantity quantity;
};

using Command = std::variant<Add, Cancel, Replace>;

struct Accepted {
    CommandKind kind;
    OrderId id;
};

struct Rejected {
    CommandKind kind;
    OrderId id;
    RejectReason reason;
};

struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Quantity quantity;
};

// Every emitted event, including rejections, consumes one sequence number.
// Accepted precedes all trades from that command; no timestamps affect ordering.
struct Event {
    Sequence sequence;
    std::variant<Accepted, Rejected, Trade> payload;
};

struct Level {
    Price price;
    Quantity total_quantity;
    std::size_t order_count;
};

// Owning copies: callers never receive iterators or references into live storage.
struct DepthSnapshot {
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// Checks fields only, in documented precedence order; stateful checks are the
// engine's responsibility. Empty optional means valid fields, not acceptance.
[[nodiscard]] std::optional<RejectReason> validate_fields(const Command& command);

} // namespace order_book
