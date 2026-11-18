# Use the local compiler with Apple's paired SDK headers and system C++ runtime.
# Mixing the downloaded libc++/ABI/unwind libraries caused exception catches to
# abort on this machine; do not add those libraries to the linker search path.
get_filename_component(ORDER_BOOK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_CXX_COMPILER "${ORDER_BOOK_ROOT}/.tools/llvm/bin/clang++")
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
    OUTPUT_VARIABLE ORDER_BOOK_SDK OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(CMAKE_OSX_SYSROOT "${ORDER_BOOK_SDK}" CACHE PATH "Apple macOS SDK")
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem \"${ORDER_BOOK_SDK}/usr/include/c++/v1\"")
