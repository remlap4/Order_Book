#pragma once

#include "order_book/order_book.hpp"
#include "book_storage.hpp"

namespace order_book {

// Internal definition lets boundary tests seed the otherwise unreachable event
// counter through a friend fixture, without a public sequence-reset operation.
struct OrderBook::Impl {
    detail::BookStorage storage;
    Sequence next_sequence = 1;
};

} // namespace order_book
