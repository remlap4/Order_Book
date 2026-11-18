# Order_Book

A C++20 limit order book and matching engine portfolio project, developed on
macOS. The focus is correctness, deterministic replay, and reproducible latency
measurements. The 28-day roadmap is maintained locally in the ignored
`requirements.txt`; the tracked contract describes the planned engine behavior.

Days 1–3 provide field validation, non-crossing adds, FIFO storage, live ID lookup,
and best-price/depth queries. Tests cover price ordering, aggregates, rejection,
and storage lifetimes. Crossing adds, cancellation, and replacement currently
throw `std::logic_error` without changing the book; matching starts on Day 4.
The demo still validates a sample command.

## Build and test on macOS

Requires Apple Clang with C++20 support, the Command Line Tools SDK, and CMake
3.20 or newer. If CMake is already on PATH, skip the local installation below.
Otherwise Python 3 can install an isolated copy inside the ignored `.tools/`:

```sh
python3 -m venv .tools/cmake-venv
.tools/cmake-venv/bin/python -m pip install cmake
export PATH="$PWD/.tools/cmake-venv/bin:$PATH"
```

From the repository root:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure
./build/debug/order_book_demo
```

For AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug -DORDER_BOOK_SANITIZERS=ON
cmake --build build/sanitize --parallel
ctest --test-dir build/sanitize --output-on-failure
```

There are no external C++ dependencies. The library target is `order_book`, the
demo is `order_book_demo`; test executables are `order_book_tests` (field contract)
and `order_book_storage_tests` (book behavior and storage).
Benchmarks arrive in Week 3; no latency claims have been measured yet.

## Repository-local compiler and editor setup

LLVM/Clang 23.1.5 for macOS ARM64 is installed in `.tools/llvm/` and ignored by Git.
This was the newest official macOS ARM64 binary available when installed;
23.1.6 was the latest release but did not yet publish a macOS binary.
Apple's compiler remains installed and can still build this project.

Use the local compiler with these presets (CMake 3.21+):

```sh
export PATH="$PWD/.tools/cmake-venv/bin:$PATH"
cmake --preset local-llvm
cmake --build --preset local-llvm --parallel
ctest --preset local-llvm
./build/local-llvm/order_book_demo
```

Substitute `local-llvm-sanitize` for all three preset names to build/test with
ASan and UBSan. Separate build directories avoid mixing compiler caches.
The macOS SDK still comes from Apple's Command Line Tools.
The toolchain file pairs local Clang with Apple SDK C++ headers and the system
C++ runtime. The previously bundled LLVM runtimes aborted on exception-catching
tests on this machine. When updating an existing local LLVM build, run
`cmake --fresh --preset local-llvm` (and the sanitizer preset) once to clear the
old linker settings. SDK discovery uses `xcrun`, so no SDK path is hard-coded.

`std::is_same_v` needs `<type_traits>` and C++17 or newer. Apple Clang 17 already
supports it; this project uses C++20. A missing-member editor diagnostic can mean
the editor is parsing in an older language mode, even when the build succeeds.
The checked-in VS Code C/C++ configuration selects the local compiler, C++20,
and the generated `build/local-llvm/compile_commands.json`. `.clangd` points
clangd-based editors at that same compilation database. Open the repository root,
configure the preset, and reload the editor's language service if errors persist.
Direct compiler commands must also include `-std=c++20` and `-Iinclude`.

The compiler binaries are local artifacts, so a fresh clone must install them
before using the presets. To reproduce this installation, download
`LLVM-23.1.5-macOS-ARM64.tar.xz` from the
[official release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.5),
verify SHA-256
`aba592b4a15e8f90c634c5367b51ec8418b8bcd2829865db331a17dc9418dcff`,
and extract into `.tools/llvm/` with one leading archive directory stripped.
The ordinary Apple Clang build instructions above remain an alternative.

Read [the engine contract](docs/contract.md) for types, error precedence, event
ordering, limits, and worked matching/replacement examples. Code comments should
explain overall logic, invariants, ownership, and non-obvious choices; avoid
restating every line. Update comments when behavior changes.
