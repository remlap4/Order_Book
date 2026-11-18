cmake_minimum_required(VERSION 3.20)
project(OrderBook VERSION 0.1.0 LANGUAGES CXX)

# Editors can consume the exact include paths and language mode used by the build.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
option(ORDER_BOOK_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)

# Keep development flags local to this project instead of modifying global flags.
add_library(order_book_options INTERFACE)
target_compile_features(order_book_options INTERFACE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(order_book_options INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
endif()
if(ORDER_BOOK_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizer configuration requires Clang or GCC")
    endif()
    target_compile_options(order_book_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(order_book_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(order_book STATIC src/validation.cpp src/order_book.cpp)
target_include_directories(order_book PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(order_book PUBLIC order_book_options)
set_target_properties(order_book PROPERTIES CXX_EXTENSIONS OFF)

# This demo exercises field validation; replay support is not yet implemented.
add_executable(order_book_demo apps/demo.cpp)
target_link_libraries(order_book_demo PRIVATE order_book)
set_target_properties(order_book_demo PROPERTIES CXX_EXTENSIONS OFF)

if(BUILD_TESTING)
    add_executable(order_book_tests tests/contract_tests.cpp)
    target_link_libraries(order_book_tests PRIVATE order_book)
    set_target_properties(order_book_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME contract COMMAND order_book_tests)
    add_executable(order_book_storage_tests tests/order_book_tests.cpp)
    target_include_directories(order_book_storage_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(order_book_storage_tests PRIVATE order_book)
    set_target_properties(order_book_storage_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME book_storage COMMAND order_book_storage_tests)
    add_test(NAME demo_smoke COMMAND order_book_demo)
    set_tests_properties(demo_smoke PROPERTIES PASS_REGULAR_EXPRESSION "Input validation: valid")
endif()
