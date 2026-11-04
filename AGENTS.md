# AGENTS.md

Guidance for AI coding agents working in this repository.

**Before writing or reviewing code, read [`docs/CODING_RULES.md`](docs/CODING_RULES.md)** — the C++ rules every
change must follow. Target architecture, deployment modes and open decisions are in [`docs/PLAN.md`](docs/PLAN.md):
read it first, it is the source of truth for the design.

Quoll is a public, header-only C++23 framework for cross-platform applications: sans-IO async runtime, HTTP client and
server, and a typed REST DSL exposed to a web UI through a generated OpenAPI contract. The same application runs as a
native server + web app, a standalone PWA (core in WASM), or a native webview shell. Namespace `quoll::`.

The maintainer communicates in French; code, comments, docs and commit messages stay in English.

## Status

Exploration. Modules so far: `quoll::async` (task, executor_ref, any_executor, await_context with stop token,
current_executor and current_stop_token, executor services, co_spawn and its `detached` handler, when_all, run_loop,
`Timer` and `timer_ref`, `sleep_for`, `with_timeout`),
`quoll::io` (`ByteStream`, `Connector`, `Listener`, `BoundListener` and `Network` concepts, `any_stream` /
`any_listener` type erasure, `io::network_ref` with `io::connect` / `io::listen` through the executor's network,
`io::network_connector`, in-memory stream pair, `io::write`), `quoll::http` (messages, llhttp parser, serializer,
`http::read` / `http::write` over a byte stream, `HttpTransport` concept, sans-IO `router`, `local_transport` to a
router of the same program; llhttp is compiled and exported as `quoll::llhttp`), `quoll::server` (`serve_connection`
serves a router on a byte stream, `serve` accepts connections from a `Listener`, `listen(router, port)`),
`quoll::client` (`transport`, an `HttpTransport` over a `Connector` with keep-alive connections, built from an `http://`
URL), `quoll::rest` (the server, `mount`, the `client` over an `HttpTransport`, `client::transport` by default, and the
contract: `endpoint`, `api`, `with`, the parts and compile-time checked path templates; and the codecs: `json_codec` on
Glaze, `text_codec`, `param_traits`), the adapters `quoll::threads` (thread pool executor; its tests do not run under
WASM), `quoll::asio` (asio executor offering the network and a timer, `tcp_stream`, TCP connector and acceptor, `runtime`;
native only, the application provides standalone asio) and `quoll::emscripten` (JavaScript event loop executor offering
a timer, `fetch_transport`; Emscripten only, its JavaScript side `js/quoll_emscripten.js` is linked with `--js-library`),
and
`quoll::runtime` (`<quoll/runtime.hpp>`: `quoll::runtime`, the runtime of the adapter chosen with `QUOLL_RUNTIME`, asio
natively; none under Emscripten). A client GET runs end to end over a memory stream, over TCP through the asio
connector, and over `fetch` in Node (prototype criteria 1 to 4); a `rest::client` built from a URL calls
`server::listen` over TCP on a `quoll::runtime`. The prototype described in `docs/PLAN.md` (OpenAPI, the React sample)
is paused while the server and the client become fully usable: see "Usable server and client" there. Do not settle its open questions or write the module design into this file before the prototype
validates them.

## Build & test

Requires CMake ≥ 3.23. Tests and install rules are ON by default when Quoll is the top-level project
(`QUOLL_BUILD_TESTS`, `QUOLL_INSTALL`).

```bash
cmake -B build
cmake --build build -j
ctest --test-dir build                # "quoll" (every test case), compile_fail.<case>, and the samples' tests
./build/tests/quoll_tests "[tag]"     # Catch2 filters
emcmake cmake -B build-wasm && cmake --build build-wasm -j && ctest --test-dir build-wasm   # runs under node
```

## Layout

Same organization as Puffin, through the helpers in `cmake/QuollHelpers.cmake`:

- Each module is a directory `quoll/<module>/` with `include/quoll/<module>/...` headers, an optional umbrella
  `include/quoll/<module>.hpp`, `tests/`, and a `CMakeLists.txt`:

  ```cmake
  quoll_declare_module(NAME http DEPS quoll::async)
  quoll_declare_tests(SOURCES tests/parser_test.cpp DEPS quoll::http)
  ```

  This creates the `INTERFACE` target `quoll_<module>`, alias `quoll::<module>`, exported as `quoll::<module>`
  by `find_package(quoll)`. Modules are added in dependency order in the root `CMakeLists.txt`.
- Adapters (thread pool, asio, later Emscripten, webview...) are modules like the others, grouped under
  `quoll/adapters/<adapter>/` (added by `quoll/adapters/CMakeLists.txt`) rather than at the root. Only the directory
  changes: the target stays `quoll::<adapter>` and the headers `<quoll/<adapter>/...>`.
  `quoll/adapters/runtime/` is no adapter: it picks the build's one (`QUOLL_RUNTIME`, asio natively), whose
  `<adapter>::runtime` becomes `quoll::runtime` in `<quoll/runtime.hpp>`; a new runtime adapter adds its branch
  there.
- All test sources compile into one executable, `build/tests/quoll_tests` (Catch2 v3 via CPM, built by
  `quoll_main_tests()` in `tests/CMakeLists.txt`).
  Helpers shared by the tests of every module live in `tests/support/quoll/test/` (`quoll::test::run` runs a
  task on a run_loop or a `virtual_time`, whose timer jumps to the next deadline instead of waiting;
  `quoll::test::overloaded`, `quoll::test::check`): use them rather than copying them into a test file.
- What must not compile (the compile-time checks of an API, `static_assert` messages included) is tested too: one source
  per case under `tests/compile_fail/`, starting with `// expect: <message>`, declared with
  `quoll_declare_compile_fail_tests(SOURCES ... DEPS ...)`. Each becomes a ctest `compile_fail.<case>` that builds it
  and passes only when the build fails with that message, taken literally (native builds only).
- `samples/<sample>/`: small programs using Quoll, built when `QUOLL_BUILD_SAMPLES` is ON (default when top-level;
  native only). `samples/notes`: a REST contract, its server and its client over TCP; with tests ON, the ctest
  `notes_round_trip` runs them against each other.
- With tests ON, every header is also compiled alone in its own translation unit (`quoll_<module>_header_check`),
  so a header that is not self-contained breaks the build.
- Dependencies come from CPM (`cmake/CPM.cmake`, always included: modules need it, not only tests). Under
  Emscripten the root sets `-fwasm-exceptions` before any dependency is added.
- Under Emscripten the test executable is linked with JSPI and run by `node --experimental-wasm-jspi`: a test waits
  for the JavaScript event loop (fetch) by suspending there with `emscripten_sleep`. Library code never does.

## Hard constraints

- Header-only, C++23 minimum (GCC ≥ 13, Clang ≥ 19). CMake, dependencies via CPM.
- WASM is a first-class target: core modules never require threads or blocking waits.
- No Qt. Adapters: thread pool (`quoll::threads`), asio (`quoll::asio`), Emscripten (`quoll::emscripten`: event
  loop, `fetch`; planned: `postMessage`); planned: webview shell, memory (tests).

## Related projects

- [Puffin](https://github.com/4thlabs/puffin) (local checkout `../puffin`): the maintainer's utility library
  (events, ioc, maths, imdb) where this plan started. Its `claude/project-thread-*` branches hold an earlier
  `async` / `webkit` / `rest` attempt, removed for quality reasons: ideas only, never port code from them.
- `cpp-init` (`../cpp-init`): the maintainer's project scaffolding tool (`npx @4thlabs/cpp-init`).

## Repository

Hosted on GitHub: [`4thlabs/quoll`](https://github.com/4thlabs/quoll). CI (`.github/workflows/ci.yml`): GCC 13, Clang 19
and Emscripten build + tests, GCC 13 with TSan and with ASan / UBSan, and a clang-format check, on every PR and push to
`main`. Claude reviews every PR (`claude-review.yml`, rules from this file and `docs/`). Formatting: `clang-format -i
<file>` with the repo `.clang-format`; format only the files you touch.
