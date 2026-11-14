# AGENTS.md

Guidance for AI coding agents working in this repository.

**Before writing or reviewing code, read [`docs/CODING_RULES.md`](docs/CODING_RULES.md)** — the C++ rules every
change must follow. The target, the decisions and the architecture are in [`docs/PLAN.md`](docs/PLAN.md): read it first, it is the
source of truth for the design; what is to do, and in which order, is in the GitHub project
[Quoll](https://github.com/users/4thlabs/projects/2). **Before committing or reviewing C++ that borrows** (a reference, pointer,
view, span, iterator or `this` held past one expression, a move, a coroutine or a callback, a destruction order that matters,
state shared between threads or tasks), run the check in
[`.agents/skills/borrow-checker/SKILL.md`](.agents/skills/borrow-checker/SKILL.md).

Quoll is a public C++23 web framework aimed at performance and ease of use: a sans-IO async runtime, an HTTP client and server,
a typed REST DSL usable from any program, and a UI DSL compiled to WASM for web, desktop and mobile, styled with Tailwind. Namespace `quoll::`.

The maintainer communicates in French; code, comments, docs and commit messages stay in English.

## Status

Exploration. The modules, each a library `quoll::<module>`:

- `quoll::async`: tasks, executors and their services, `co_spawn`, `when_all`, timers, stops, blocking work offloaded.
- `quoll::io`: byte streams, listeners, connectors and networks, TLS options, their type erasure, a memory stream.
- `quoll::http`: messages, the llhttp parser (llhttp compiled and exported as `quoll::llhttp`), the serializer, the
  sans-IO router, server-sent events and WebSocket.
- `quoll::server` and `quoll::client`: `http::server` and `http::client`, their names in `quoll::http`.
- `quoll::rest`: the contract DSL, `mount`, `rest::client`, the codecs.
- `quoll::ui`: components in two forms, states, effects and actions, forms, contexts, routing, widgets, `to_html`.
- The adapters `quoll::asio` (native) and `quoll::emscripten` (the browser), and `quoll::runtime`, the build's one.

What each gives is in the module table of [`README.md`](README.md#what-it-holds) and in the guides under `docs/`. Do not
write the module design into this file. **What is to do, and its priority, is in the GitHub project
[Quoll](https://github.com/users/4thlabs/projects/2); the design and the decisions are in `docs/PLAN.md`.**

## Build & test

Requires CMake ≥ 3.23. Tests and install rules are ON by default when Quoll is the top-level project
(`QUOLL_BUILD_TESTS`, `QUOLL_INSTALL`). The tests run in a second, the time goes to compiling: filter them while working,
and leave the header checks to CI with `-DQUOLL_HEADER_CHECKS=OFF`. `QUOLL_TLS` (`AUTO`, `ON`, `OFF`) says whether TLS
goes through OpenSSL.

```bash
cmake -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build -j"$(nproc)"   # "quoll" (every test case but the hidden ones), compile_fail.<case>, assert.<name>, the samples' tests
./build/tests/quoll_tests "[tag]"     # Catch2 filters
emcmake cmake -B build-wasm && cmake --build build-wasm -j"$(nproc)" && ctest --test-dir build-wasm   # runs under node
run-clang-tidy-19 -p build -quiet "$PWD/(quoll|samples)/.*\.cpp$"   # needs a Clang build; all of it, as every night: a PR leaves the analyzer out of the tests
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DQUOLL_BENCH=ON -DQUOLL_BUILD_TESTS=OFF && cmake --build build-bench --target quoll_bench -j"$(nproc)" \
  && build-bench/tests/bench/quoll_bench tcp   # what it costs: docs/BENCHMARKS.md
# A load under TSan, as load.yml runs it. GCC 13's TSan needs `sudo sysctl vm.mmap_rnd_bits=28` where it is higher.
cmake -B build-load -DCMAKE_BUILD_TYPE=Debug -DQUOLL_LOAD=ON -DQUOLL_BUILD_TESTS=OFF -DQUOLL_BUILD_SAMPLES=OFF \
  "-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer" "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -O1" \
  && cmake --build build-load --target quoll_load -j"$(nproc)" && build-load/tests/load/quoll_load 30
CC=clang-19 CXX=clang++-19 cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQUOLL_FUZZ=ON -DQUOLL_BUILD_TESTS=OFF -DQUOLL_BUILD_SAMPLES=OFF \
  && cmake --build build-fuzz --target quoll_fuzzers -j"$(nproc)" && tests/fuzz/run.sh build-fuzz 60   # libFuzzer, 60 s a target
```

## Layout

Same organization as Puffin, through the helpers in `cmake/QuollHelpers.cmake`; what
[`docs/development.md`](docs/development.md#layout) says of it is not repeated here:

- Each module is a directory `quoll/<module>/`, declared with `quoll_declare_module` and `quoll_declare_tests`, and
  added in dependency order in the root `CMakeLists.txt`. `quoll::ui` has its headers in folders by subject; the other
  modules stay flat for now. Adapters are modules grouped under `quoll/adapters/<adapter>/`, their target
  `quoll::<adapter>` and their headers `<quoll/<adapter>/...>`. `quoll/adapters/runtime/` is no adapter: it picks the
  build's one (`QUOLL_RUNTIME`), whose `<adapter>::runtime` becomes `quoll::runtime`; a new runtime adapter adds its
  branch there.
- All test sources compile into one executable, `build/tests/quoll_tests` (Catch2 v3 via CPM, built by
  `quoll_main_tests()` in `tests/CMakeLists.txt`).
  Helpers shared by the tests of every module live in `tests/support/quoll/test/` (`quoll::test::run` runs a
  task on a run_loop or a `virtual_time`, whose timer jumps to the next deadline instead of waiting;
  `quoll::test::memory_renderer`, `quoll::test::memory_server` and `memory_connector`, an `http::client` reaching an `http::server` over memory streams, `quoll::test::overloaded`, `quoll::test::check`, `quoll::test::code_thrown_by`, the code of what a task throws, `quoll::test::next_websocket_message`, for a peer that speaks bytes, and `quoll::test::websocket_echo`): use them rather than copying them into a test file.
- What must not compile (the compile-time checks of an API, `static_assert` messages included) is tested too: one source
  per case under `tests/compile_fail/`, starting with `// expect: <message>`, declared with
  `quoll_declare_compile_fail_tests(SOURCES ... DEPS ...)`. Each becomes a ctest `compile_fail.<case>` that compiles it
  and passes only when the compilation fails with that message, taken literally (native builds only).
- What a debug build asserts, an invariant out of a caller's reach (a task destroyed while it runs, which a program cannot
  reach through `co_await` and `co_spawn`), is tested too: a hidden
  Catch2 case (`[.assert]`) that breaks it, declared with `quoll_declare_assert_tests(NAME ... CASE ... EXPECT ...)`. Each
  becomes a ctest `assert.<name>` that runs it alone and passes only when the program ends with the assertion's text, which
  the case's name must not hold (native builds; a build without assertions says so and passes).
- `tests/fuzz/`, `tests/bench/`, `tests/load/` and `tests/interop/`: libFuzzer targets, the benchmark, a mixed load on a
  server and its clients, and runs against third-party peers, each built by its own option rather than in `quoll_tests`,
  and run every night and on a PR touching it: see [`docs/development.md`](docs/development.md#what-ci-checks). An input
  that crashed a fuzz target joins its seeds in `tests/fuzz/corpus/<target>/`, with a test of the fix. `bench.yml`
  checks the allocations per request against the budgets in the benchmark's source, GCC 13's, which do not depend on
  the machine: a change that raises one says why. CI compiles `quoll_load`, so that a change to what it uses breaks the
  PR rather than the night.
- `samples/<sample>/`: small programs using Quoll, built when `QUOLL_BUILD_SAMPLES` is ON (default when top-level).
  `samples/notes`: a REST contract in `api/`, its server in `server/` and its command line in `cli/` (native), and its
  page in `web/` (Emscripten); with tests ON, the ctest `notes_round_trip` runs the server and the command line against
  each other.
  `samples/dashboard`: a screen of an application over a products contract, its server (native) and its page (Emscripten):
  a list to filter, a detail to edit and a chart.js widget; its service and its page are tested in `quoll_tests`.
  `samples/github`: a command-line program calling GitHub's API, natively and in Node through `fetch`; the ctest
  `github_against_fake` runs it against a fake GitHub served by Quoll.
- `tools/vite-plugin/`: the Vite plugin a page's build goes through: it builds the C++ with CMake, serves and reloads it,
  and makes the registry of its JavaScript widgets; and `dev.sh`, which a page's `dev.sh` runs with its server. Under
  Emscripten, the ctest `vite_plugin` runs its tests in Node.
- With tests ON, every header is also compiled alone in its own translation unit (`quoll_<module>_header_check`),
  so a header that is not self-contained breaks the build (`QUOLL_HEADER_CHECKS`, ON by default).
- `docs/`: the guides for users, one Markdown file per subject, listed in the `README.md`, beside `PLAN.md`, `CODING_RULES.md` and `BENCHMARKS.md`;
  `docs/design/` holds a design note per DSL, what was decided with the maintainer and where it stands.
  The guides say how things behave, without arguing for them; `design-decisions.md` says why, for users, from the Decided
  section of `PLAN.md`: a change to a decision there updates it, and the guide it touches, in the same pull request.
- Dependencies come from CPM (`cmake/CPM.cmake`, always included: modules need it, not only tests). Under
  Emscripten the root sets `-fwasm-exceptions` and `-mtail-call` before any dependency is added, and each module passes them on to the program: coroutines resuming each other otherwise grow the stack.
- Under Emscripten the test executable is linked with JSPI and run by `node --experimental-wasm-jspi`: a test waits
  for the JavaScript event loop (fetch) by suspending there with `emscripten_sleep`. Library code never does.

## Hard constraints

- Compiled libraries, with templates over what an application provides in headers. C++23 minimum (GCC ≥ 13, Clang ≥ 19, Apple Clang ≥ 21). CMake, dependencies
  via CPM.
- WASM is a first-class target: core modules never require threads or blocking waits.
- Adapters: asio (`quoll::asio`), Emscripten (`quoll::emscripten`: event loop, `fetch`, the
  DOM); planned: a webview shell.

## Related projects

- [Puffin](https://github.com/4thlabs/puffin) (local checkout `../puffin`): the maintainer's utility library
  (events, ioc, maths, imdb) where this plan started. Its `claude/project-thread-*` branches hold an earlier
  `async` / `webkit` / `rest` attempt, removed for quality reasons: ideas only, never port code from them.
- `cpp-init` (`../cpp-init`): the maintainer's project scaffolding tool (`npx @4thlabs/cpp-init`).

## Repository

Hosted on GitHub: [`4thlabs/quoll`](https://github.com/4thlabs/quoll). CI (`.github/workflows/ci.yml`) runs on every PR
and push to `main`; fuzzing, the load, the interop runs, the benchmark and the whole of `clang-tidy` run every night, and
on a PR touching their targets or rules. What each job checks is in
[`docs/development.md`](docs/development.md#what-ci-checks). Formatting: `clang-format-18 -i <file>` with the repo `.clang-format`; format only
the files you touch, but for a change to `.clang-format`, which formats the whole repository in a commit of its own, listed in
`.git-blame-ignore-revs`.

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
is not taken, its threads then resolved; and, when a kind of bug came back in its reviews, once the maintainer has
answered, as said below.

What a review finds is fixed if valid: a bug, a break of a rule or a decision, a missing test, or one that does not execute what
it claims. A detail may be let go for a stated reason, and needs no new push: naming, layout and wording with no consequence,
in samples and the sample's Vite plugin a bug that normal use does not meet, and in the HTTP protocol an RFC corner that touches
neither security nor stability (`docs/PLAN.md`, Decided), but for undefined behaviour, lifetimes and data
races, which are always fixed. A finding that is wrong says why. When it is unclear which of these a point is, for one saying it
is a bug in particular, the maintainer decides; such an RFC corner, even labelled a bug, needs no decision of theirs.

When a round of review on a pull request, local or Codex's, finds a kind of bug an earlier round on it found, the feature is
what to change rather than each case. Name the kind, see what the references of its layer give for it (Beast, hyper and Go's
net/http for HTTP, Capy and C++26's `std::execution` for `quoll::async`, Leptos, Dioxus, Solid and React Query for
`quoll::ui`), and propose the change in Quoll to the maintainer, with the code a user would write, under Open questions in
`docs/PLAN.md` or in the issue of its category. That pull request waits for their answer. What it fixed meanwhile, in
Quoll or using it, stays as simple as it can and says in a comment what will replace it, with that entry or issue. Code
stays understandable: a simpler design is worth more than a guard that covers one more case.

If Codex has not completed a review of the last commit 30 minutes after the push or the opening of the PR, whichever is later,
comment `@codex review`; 30 minutes later, the maintainer decides, who alone can lift the protection.

The maintainer merges a big architecture change, even with every check green: a new module, a change to a hard constraint
above, a redesign of how modules fit together, and any change to this rule, to `REVIEW.md` or to
`codex-review.yml`. In doubt, it counts as one.

## Code Review Rules

Read [`REVIEW.md`](REVIEW.md) and follow it for every review: it holds these rules. In short, report everything you find, the
small points too, as P1 at least, saying what each one really is.
