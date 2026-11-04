# Quoll coding rules

Quoll is a **modern C++ header-only library, C++23 minimum** (GCC ≥ 13, Clang ≥ 19, Apple Clang ≥ 21). Rules follow the Boost library
guidelines and the C++ Core Guidelines.

## Header-only rules

- Every header is self-contained (compiles when included alone) and includes what it uses, nothing more. Prefer
  `<iosfwd>` / forward declarations to heavy includes; no `<iostream>` in library headers.
- No ODR violations: non-template free functions and out-of-class member definitions are `inline`, namespace-scope
  constants are `inline constexpr`, no `static` or unnamed-namespace entities and no mutable global state in headers.
- Never `using namespace` at namespace scope in a header.
- A module lives in `quoll/<module>/`, or in `quoll/adapters/<module>/` for an adapter (thread pool, asio...); paths
  below are relative to that directory.
- Implementation details live in `quoll::<module>::detail` (files under `include/quoll/<module>/detail/`). A user
  never has to name anything in `detail`.
- Public API: one header per component plus an umbrella `include/quoll/<module>.hpp`.
- No `.cpp`, no explicit instantiation / `extern template`. Third-party C/C++ dependencies (e.g. llhttp) may be
  compiled targets fetched by CPM; Quoll's own code stays header-only.
- Macros only for include guards and `QUOLL_`-prefixed configuration switches.

## Layout & naming (Boost style)

- Namespace `quoll::<module>`, nested form: `namespace quoll::http {` ... `} // namespace quoll::http`.
- Include guards (not `#pragma once`): `QUOLL_<MODULE>_<PATH>_<FILE>_HPP`, closed with the same name in a comment.
- Includes use angle brackets and the full path: `#include <quoll/async/executor.hpp>`.
- `snake_case` for types, functions, variables, namespaces and files, like the standard library. `PascalCase` for
  template parameters and concepts (`CharT`, `Allocator`, `Executor`). Macros in `QUOLL_UPPER_CASE`.
- Private data members end with `_` (`handle_`); public members of aggregates don't.
- Standard-like member aliases (`value_type`, `size_type`, `iterator`, `executor_type`...). Text types are generic
  over the character type: `basic_xxx<CharT, Traits = std::char_traits<CharT>, ...>` + `using xxx = basic_xxx<char>;`.
- Every C++ file (headers, tests) starts with the Quoll ASCII logo + BSD 3-Clause banner (current year); CMake
  files don't:

```cpp
//   ___              _ _
//  / _ \ _   _  ___ | | |
// | | | | | | |/ _ \| | |
// | |_| | |_| | (_) | | |
//  \__\_\\__,_|\___/|_|_|
//
// BSD 3-Clause License
//
// Copyright (c) 2026, Thomas Gourgues (thomas.gourgues@gmail.com)
// All rights reserved.
//
// (remaining clauses verbatim from LICENSE.md, each line prefixed with "// ")
```

## Modern C++

- Constrain templates with concepts / `requires`; no SFINAE or `enable_if`. Each concept has its own header, and each
  model is checked with `static_assert(Concept<Model>);`.
- `constexpr` wherever possible, `consteval` for compile-time-only helpers.
- Non-owning parameters as `std::string_view` / `std::span`; ownership through values or `std::unique_ptr`. No raw
  owning pointers, no `new` / `delete`. Throw by value, catch by `const&`.
- Rule of zero. A type owning a resource (coroutine handle, thread...) explicitly defines or deletes all five
  special members, in the public section; move operations are `noexcept`.
- `[[nodiscard]]` on observers, factories and awaitables; `noexcept` when it cannot throw; `explicit` on
  single-argument and conversion constructors; observers are `const`.
- Static polymorphism (templates + concepts) by default; type erasure (see below) when runtime polymorphism is
  required. No `virtual` in value types.
- Use the standard library: `std::optional`, `std::expected`, `std::variant`, `std::invoke`, `std::ranges`.
- Concurrency: document the thread-safety of each class; prefer `std::jthread` + `std::stop_token`; a destructor never
  leaves a joinable thread behind. The core modules never require threads (WASM, see `PLAN.md`).
- Errors: exceptions derived from standard exception types for failures the caller cannot anticipate;
  `std::optional` (or an error-code return) when failure is an expected outcome (e.g. parsing user input).

## Type erasure

- Non-intrusive: the erased type never has to derive from a base class; anything modelling the concept can be
  wrapped, including third-party types (asio executors, browser APIs).
- No `virtual`. Use a struct of function pointers, with one `inline constexpr` instance per erased type built from
  captureless lambdas. The table may also carry data (e.g. size and alignment of the awaitable to preallocate).
  A wrapper exposing a single operation may store the function pointer directly instead of a table.
- Two flavours, sharing the same table:
  - `xxx_ref`: non-owning, `void*` + table pointer, trivially copyable, never allocates. The referenced object must
    outlive it.
  - `any_xxx`: owning, small-buffer storage with heap fallback; copy / move / destroy are entries of the table.
- Awaitables returned by erased async operations are constructed in storage preallocated by the wrapper: no
  allocation per operation in steady state.
- Erased wrappers model the concept they erase (an `executor_ref` is an executor) and offer `target<T>()` to recover
  the concrete type, using a library type id rather than RTTI.

## Readability

- One component per header; short functions with early returns, no deep nesting.
- Air out function bodies: a blank line after the local declarations, and around every control block (`if` /
  `else`, loops, `try` / `catch`) that has statements before or after it, at the same level. clang-format keeps these
  lines but does not add them.
- Factor what template specializations share into a `detail` base instead of duplicating it (e.g. `task<T>` /
  `task<void>`).
- Descriptive names (`handle`, `executor`, `parent`) except conventional ones (`T`, `It`, loop indices).
- No commented-out code, empty stubs or placeholder classes in committed code.
- Doxygen `/** @brief ... */` on the public API; comments explain *why*, not *what*.

## Tests

- Every public header is included by at least one test TU, so it is compiled.
- Tests must actually execute the behaviour they claim to test: a coroutine test runs its executor until the
  coroutine completes, and checks the result.
- Test files use Catch2 v3 headers (`<catch2/catch_test_macros.hpp>`).
