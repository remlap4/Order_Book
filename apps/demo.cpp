#include "order_book/types.hpp"

#include <iostream>

int main() {
    // Exercise the linked library without implying that matching already exists.
    const order_book::Command command = order_book::Add{
        1, order_book::Side::buy, 100, 10};
    const auto error = order_book::validate_fields(command);
    std::cout << "Order_Book: field validation demo (matching not implemented)\n"
              << "Input validation: " << (error ? "invalid" : "valid") << '\n';
    return error ? 1 : 0;
}
