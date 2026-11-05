# AGENTS.md

Guidance for AI coding agents working in this repository.

**Before writing or reviewing code, read [`docs/CODING_RULES.md`](docs/CODING_RULES.md)** — the C++ rules every
change must follow. The target, the roadmap and open decisions are in [`docs/PLAN.md`](docs/PLAN.md): read it first, it is the
source of truth for the design and the order of the work.

Quoll is a public C++23 web framework aimed at performance and ease of use: a sans-IO async runtime, an HTTP client and server,
a typed REST DSL usable from any program, and a UI DSL compiled to WASM for web, desktop and mobile, styled with Tailwind. Namespace `quoll::`.

The maintainer communicates in French; code, comments, docs and commit messages stay in English.

## Status

Exploration. Modules so far: `quoll::async` (task, executor_ref, any_executor, await_context with stop token,
current_executor and current_stop_token, executor services, co_spawn and its `detached` handler, when_all, run_loop,
`Timer` and `timer_ref`, `sleep_for`, `with_timeout`),
`quoll::io` (`ByteStream`, `HalfClosableStream`, `Connector`, `Listener`, `BoundListener` and `Network` concepts, `any_stream` /
`any_listener` type erasure, `io::network_ref` with `io::connect` / `io::listen` through the executor's network,
`io::network_connector`, TLS options with `TlsNetwork`, `tls_network_ref`, `TlsConnector`, `io::connect_tls` and
`io::listen_tls`, in-memory stream pair, `io::write`), `quoll::http` (messages, llhttp parser, serializer,
`http::read` / `http::write` over a byte stream, `HttpTransport` concept, sans-IO `router`, `local_transport` to a
router of the same program; llhttp is compiled and exported as `quoll::llhttp`), `quoll::server` (`serve_connection`
serves a router on a byte stream, `serve` accepts connections from a `Listener`, `listen(router, port)`),
`quoll::client` (`transport`, an `HttpTransport` over a `Connector` with keep-alive connections, built from an
`http[s]://` URL), `quoll::rest` (the server, `mount`, the `client` over an `HttpTransport`, `client::transport` by default, and the
contract: `endpoint`, `api`, `with`, the parts and compile-time checked path templates; and the codecs: `json_codec` on
Glaze, `text_codec`, `param_traits`), `quoll::ui` (a prototype: elements, decorators, `state`, `to_html`, `app` and the `Renderer`
concept), the adapters `quoll::threads` (thread pool executor; its tests do not run under
WASM), `quoll::asio` (asio executor offering the network, a timer and TLS, `tcp_stream`, `tls_stream`, connector and acceptors,
`runtime`; native only, its runtime hides asio from a program, its streams and acceptors need standalone asio, TLS needs OpenSSL) and `quoll::emscripten` (JavaScript event loop executor offering
a timer, `fetch_transport`, `dom_renderer` and `mount`; Emscripten only, its JavaScript side `js/quoll_emscripten.js` is linked with `--js-library`),
and
`quoll::runtime` (`<quoll/runtime.hpp>`: `quoll::runtime`, the runtime of the adapter chosen with `QUOLL_RUNTIME`, asio
natively; none under Emscripten). A client GET runs end to end over a memory stream, over TCP through the asio
connector, and over `fetch` in Node; a `rest::client` built from a URL calls `server::listen` over TCP on a `quoll::runtime`.
**The roadmap in `docs/PLAN.md` says what comes next, and which step is current.** Do not write the module design into this file.

## Build & test

Requires CMake ≥ 3.23. Tests and install rules are ON by default when Quoll is the top-level project
(`QUOLL_BUILD_TESTS`, `QUOLL_INSTALL`). The tests run in a second, the time goes to compiling: filter them while working,
and leave the header checks to CI with `-DQUOLL_HEADER_CHECKS=OFF`. `QUOLL_TLS` (`AUTO`, `ON`, `OFF`) says whether TLS
goes through OpenSSL.

```bash
cmake -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build -j"$(nproc)"   # "quoll" (every test case), compile_fail.<case>, and the samples' tests
./build/tests/quoll_tests "[tag]"     # Catch2 filters
emcmake cmake -B build-wasm && cmake --build build-wasm -j"$(nproc)" && ctest --test-dir build-wasm   # runs under node
run-clang-tidy-19 -p build -quiet "$PWD/(quoll|samples)/.*\.cpp$"   # needs a Clang build; the same as CI
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DQUOLL_BENCH=ON -DQUOLL_BUILD_TESTS=OFF && cmake --build build-bench --target quoll_bench -j"$(nproc)" \
  && build-bench/tests/bench/quoll_bench tcp   # what it costs: docs/BENCHMARKS.md
CC=clang-19 CXX=clang++-19 cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQUOLL_FUZZ=ON -DQUOLL_BUILD_TESTS=OFF -DQUOLL_BUILD_SAMPLES=OFF \
  && cmake --build build-fuzz --target quoll_fuzzers -j"$(nproc)" && tests/fuzz/run.sh build-fuzz 60   # libFuzzer, 60 s a target
```

## Layout

Same organization as Puffin, through the helpers in `cmake/QuollHelpers.cmake`:

- Each module is a directory `quoll/<module>/` with `include/quoll/<module>/...` headers, an optional umbrella
  `include/quoll/<module>.hpp`, `src/` once it has code compiled once, `tests/`, and a `CMakeLists.txt`:

  ```cmake
  quoll_declare_module(NAME http SOURCES src/parser.cpp DEPS quoll::async)
  quoll_declare_tests(SOURCES tests/parser_test.cpp DEPS quoll::http)
  ```

  This creates the target `quoll_<module>`, a static library, or `INTERFACE` without `SOURCES`, alias `quoll::<module>`, exported as `quoll::<module>`
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
  `quoll::test::memory_renderer`, `quoll::test::overloaded`, `quoll::test::check`): use them rather than copying them into a test file.
- What must not compile (the compile-time checks of an API, `static_assert` messages included) is tested too: one source
  per case under `tests/compile_fail/`, starting with `// expect: <message>`, declared with
  `quoll_declare_compile_fail_tests(SOURCES ... DEPS ...)`. Each becomes a ctest `compile_fail.<case>` that compiles it
  and passes only when the compilation fails with that message, taken literally (native builds only).
- `tests/fuzz/`: one libFuzzer target per source, built with `QUOLL_FUZZ` (Clang, under ASan and UBSan) rather than in
  `quoll_tests`, its seeds in `corpus/<target>/`. An input that crashed one joins its seeds, with a test of the fix.
- `tests/bench/`: `quoll_bench`, built with `QUOLL_BENCH`, measures requests per second, latency and allocations, its figures in
  `docs/BENCHMARKS.md`. `bench.yml` runs it every night, and on a PR touching it only, and checks the allocations per request against
  the budgets in its source, GCC 13's, which do not depend on the machine. A change that raises one says why.
- `samples/<sample>/`: small programs using Quoll, built when `QUOLL_BUILD_SAMPLES` is ON (default when top-level).
  `samples/notes` (native): a REST contract, its server and its client over TCP; with tests ON, the ctest
  `notes_round_trip` runs them against each other. `samples/ui_notes` (Emscripten): the same contract in a browser.
- With tests ON, every header is also compiled alone in its own translation unit (`quoll_<module>_header_check`),
  so a header that is not self-contained breaks the build (`QUOLL_HEADER_CHECKS`, ON by default).
- Dependencies come from CPM (`cmake/CPM.cmake`, always included: modules need it, not only tests). Under
  Emscripten the root sets `-fwasm-exceptions` before any dependency is added.
- Under Emscripten the test executable is linked with JSPI and run by `node --experimental-wasm-jspi`: a test waits
  for the JavaScript event loop (fetch) by suspending there with `emscripten_sleep`. Library code never does.

## Hard constraints

- Compiled libraries, with templates over what an application provides in headers. C++23 minimum (GCC ≥ 13, Clang ≥ 19, Apple Clang ≥ 21). CMake, dependencies
  via CPM.
- WASM is a first-class target: core modules never require threads or blocking waits.
- Adapters: thread pool (`quoll::threads`), asio (`quoll::asio`), Emscripten (`quoll::emscripten`: event loop, `fetch`, the
  DOM); planned: a webview shell.

## Related projects

- [Puffin](https://github.com/4thlabs/puffin) (local checkout `../puffin`): the maintainer's utility library
  (events, ioc, maths, imdb) where this plan started. Its `claude/project-thread-*` branches hold an earlier
  `async` / `webkit` / `rest` attempt, removed for quality reasons: ideas only, never port code from them.
- `cpp-init` (`../cpp-init`): the maintainer's project scaffolding tool (`npx @4thlabs/cpp-init`).

## Repository

Hosted on GitHub: [`4thlabs/quoll`](https://github.com/4thlabs/quoll). CI (`.github/workflows/ci.yml`): GCC 13, Clang 19
and Emscripten build + tests, GCC 13 with TSan and with ASan / UBSan, and a clang-format check, on every PR and push to
`main`, and Apple Clang on macOS; fuzzing (`.github/workflows/fuzz.yml`) runs every night, and on a PR touching its targets; the header checks run on GCC 13, Emscripten and macOS, the `compile_fail` ctests on GCC 13 and Clang 19, `clang-tidy`
(`.clang-tidy`) on Clang 19, and the jobs keep their compilations in ccache. Formatting: `clang-format -i <file>` with the repo `.clang-format`; format only
the files you touch.

`main` is protected: everything goes through a PR, with every required CI job and the status `Codex` green, up to date with
`main` and its threads resolved, with no bypass for admins.

Codex reviews a PR when it opens and after each later push, on its own (the ChatGPT Codex Connector, set up in Codex's settings,
following [`REVIEW.md`](REVIEW.md)). `codex-review.yml` sets the status `Codex` once it has finished reviewing the last commit.

Claude Code may merge the PRs it opened, giving the commit it checked, on one condition: `/code-review high` runs before
every push, except a clean merge of `main`, and its valid findings are fixed first, but for what the rule below lets go.

```bash
gh pr merge N -R 4thlabs/quoll --rebase --delete-branch --match-head-commit <sha>
# If GitHub refuses the rebase. The default body lists every commit.
gh pr merge N -R 4thlabs/quoll --squash --delete-branch --match-head-commit <sha> --body "<what the PR does>"
```

The PR says what that review found and why any finding was not taken. A PR merges once every required check is green on
its last commit, Codex has finished on it, and all it found on the PR is answered, with the fixing commit or the reason a point
is not taken, its threads then resolved.

What a review finds is fixed if valid: a bug, a break of a rule or a decision, a missing test, or one that does not execute what
it claims. A detail may be let go for a stated reason, and needs no new push: naming, layout and wording with no consequence, and
in samples and the sample's Vite plugin a bug that normal use does not meet, but for undefined behaviour, lifetimes and data
races, which are always fixed. A finding that is wrong says why. When it is unclear which of these a point is, for one saying it
is a bug in particular, the maintainer decides.

If Codex has not completed a review of the last commit 30 minutes after the push or the opening of the PR, whichever is later,
comment `@codex review`; 30 minutes later, the maintainer decides, who alone can lift the protection.

The maintainer merges a big architecture change, even with every check green: a new module, a change to a hard constraint
above, a redesign of how modules fit together, and any change to this rule, to `REVIEW.md` or to
`codex-review.yml`. In doubt, it counts as one.

## Code Review Rules

Read [`REVIEW.md`](REVIEW.md) and follow it for every review: it holds these rules. In short, report everything you find, the
small points too, as P1 at least, saying what each one really is.
