#include "order_book/types.hpp"

#include <type_traits>

namespace order_book {

std::optional<RejectReason> validate_fields(const Command& command) {
    // Field precedence is stable for inputs with several errors. Signed text
    // parsing must happen before constructing unsigned IDs or quantities.
    return std::visit([](const auto& value) -> std::optional<RejectReason> {
        using T = std::decay_t<decltype(value)>;
        if (value.id == 0) {
            return RejectReason::invalid_id;
        }
        if constexpr (std::is_same_v<T, Add>) {
            if (value.side != Side::buy && value.side != Side::sell) {
                return RejectReason::invalid_side;
            }
        }
        if constexpr (!std::is_same_v<T, Cancel>) {
            if (value.price <= 0 || value.price > max_price) {
                return RejectReason::invalid_price;
            }
            if (value.quantity == 0 || value.quantity > max_quantity) {
                return RejectReason::invalid_quantity;
            }
        }
        return std::nullopt;
    }, command);
}

} // namespace order_book
