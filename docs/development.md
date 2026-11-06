# Development

## Setup

You need CMake 3.23 and a C++23 compiler: GCC 13, Clang 19 or Apple Clang 21 at least. OpenSSL brings
TLS. The dev container in `.devcontainer/` has it all: GCC 13, Clang 19, CMake, Ninja, ccache, OpenSSL,
the Emscripten SDK and Node.

```bash
git clone https://github.com/4thlabs/quoll.git && cd quoll
cmake -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build -j"$(nproc)"
```

Dependencies come from CPM. Set `CPM_SOURCE_CACHE` to keep them between builds.

| Option                | Default         | What it does                                          |
| --------------------- | --------------- | ----------------------------------------------------- |
| `QUOLL_BUILD_TESTS`   | ON at top level | The tests                                             |
| `QUOLL_HEADER_CHECKS` | ON              | With the tests, compile every header alone            |
| `QUOLL_BUILD_SAMPLES` | ON at top level | `samples/`                                            |
| `QUOLL_INSTALL`       | ON at top level | The install rules                                     |
| `QUOLL_TLS`           | `AUTO`          | TLS through OpenSSL: `AUTO`, `ON` or `OFF`            |
| `QUOLL_RUNTIME`       | per platform    | The adapter behind `quoll::runtime`                   |
| `QUOLL_UI_HOT`        | OFF             | `quoll::ui`: literals replaced while the program runs |
| `QUOLL_BENCH`         | OFF             | `tests/bench`, without the tests                      |
| `QUOLL_FUZZ`          | OFF             | `tests/fuzz`, with Clang                              |

## Tests

The tests run in a second, and the time goes to compiling. So filter them while working, and leave the
header checks to CI:

```bash
cmake -B build -DQUOLL_HEADER_CHECKS=OFF
cmake --build build --target quoll_tests -j"$(nproc)"
./build/tests/quoll_tests "[client]"     # Catch2 filters
```

`ctest` runs `quoll` (every test case), `compile_fail.<case>` and the samples' tests.

- **Tests** of a module are in `quoll/<module>/tests/`, compiled together into `build/tests/quoll_tests`.
  A test must execute what it claims: a coroutine test runs its executor until the coroutine completes,
  and checks the result. Helpers shared by the tests of every module are in
  `tests/support/quoll/test/`, such as `quoll::test::run`, which runs a task on a `run_loop` or on a
  `virtual_time`, whose timer jumps to the next deadline instead of waiting.
- **What must not compile** is tested too: one source per case under a module's
  `tests/compile_fail/`, starting with `// expect: <message>`. Its ctest passes only when the compilation
  fails with that message, taken literally (native builds only).
- **Every header** is compiled alone in its own translation unit, so that one that is not self-contained
  breaks the build.

### WebAssembly

```bash
source /opt/emsdk/emsdk_env.sh    # in the dev container
emcmake cmake -B build-wasm && cmake --build build-wasm -j"$(nproc)" && ctest --test-dir build-wasm
```

The tests run under Node, with JSPI. A test waits for the event loop by suspending there with
`emscripten_sleep`. Library code never does.

### Other checks

```bash
# clang-tidy, as it runs every night: needs a Clang build. On a pull request, the tests go without its analyzer
run-clang-tidy-19 -p build -quiet "$PWD/(quoll|samples)/.*\.cpp$"

# libFuzzer, under ASan and UBSan: 60 s a target
CC=clang-19 CXX=clang++-19 cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQUOLL_FUZZ=ON \
  -DQUOLL_BUILD_TESTS=OFF -DQUOLL_BUILD_SAMPLES=OFF
cmake --build build-fuzz --target quoll_fuzzers -j"$(nproc)"
tests/fuzz/run.sh build-fuzz 60

# What it costs: see performance.md
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DQUOLL_BENCH=ON -DQUOLL_BUILD_TESTS=OFF
cmake --build build-bench --target quoll_bench -j"$(nproc)"
build-bench/tests/bench/quoll_bench allocations --check
```

Format what you touch, and only that, with the repository's `.clang-format`:

```bash
clang-format -i <file>
```

## Layout

```
quoll/
├── async/      # task, executors, co_spawn, when_all, timers
├── io/         # byte streams, listeners, connectors, TLS options
├── http/       # messages, parser, serializer, router
├── server/     # http::server, static_files
├── client/     # http::client
├── rest/       # contracts, mount, client, codecs
├── ui/         # the UI prototype
└── adapters/   # threads, asio, emscripten, runtime
samples/        # notes (native), ui_notes (Emscripten)
tests/          # the test program, its helpers, fuzz/, bench/
docs/           # these guides, the plan, the rules
```

A module is a directory with `include/quoll/<module>/...`, an optional umbrella
`include/quoll/<module>.hpp`, `src/` once it has code compiled once, `tests/`, and a `CMakeLists.txt`:

```cmake
quoll_declare_module(NAME http SOURCES src/parser.cpp DEPS quoll::async)
quoll_declare_tests(SOURCES tests/parser_test.cpp DEPS quoll::http)
```

This creates the target `quoll_<module>`, a static library, or `INTERFACE` without `SOURCES`, alias
`quoll::<module>`, exported by `find_package(quoll)`. Modules are added in dependency order in the root
`CMakeLists.txt`. Adapters are modules like the others, grouped under `quoll/adapters/`.

Under Emscripten, the root sets `-fwasm-exceptions` and `-mtail-call` before any dependency is added,
and each module passes them on to the program.

## What CI checks

Every pull request and push to `main` goes through `.github/workflows/ci.yml`:

| Job                     | What it does                                                             |
| ----------------------- | ------------------------------------------------------------------------ |
| GCC 13                  | Build, tests, header checks and `compile_fail`, then a build without TLS |
| Clang 19                | Build, tests, `compile_fail`, `clang-tidy` without its analyzer on tests |
| GCC 13 with TSan        | Build and tests                                                          |
| GCC 13 with ASan, UBSan | Build and tests                                                          |
| Emscripten              | Build and tests under Node, header checks                                |
| macOS                   | Build and tests with Apple Clang, header checks                          |
| clang-format            | Every `.hpp` and `.cpp`, with `clang-format-18`                          |

Fuzzing (`fuzz.yml`), the benchmark (`bench.yml`) and all of `clang-tidy` (`clang-tidy.yml`) run every night, and on a pull
request touching them.
`codex-review.yml` sets the status `Codex`, once Codex has finished reviewing the last commit.

## Contributing

- **Read first:** [`CODING_RULES.md`](CODING_RULES.md), the C++ rules every change follows, and
  [`PLAN.md`](PLAN.md), the target, the roadmap and the decisions made. The roadmap says which step is
  current. A new public API is shown to the maintainer before much is built on it.
- **One pull request per topic.** `main` is protected: everything goes through a pull request, with
  every required CI job and the status `Codex` green, up to date with `main`, and its threads resolved.
- **Reviews.** Codex reviews a pull request when it opens and after each push, following
  [`REVIEW.md`](../REVIEW.md), which holds the rules of a review for whoever does one. Each finding is
  answered, with the fixing commit or the reason it is not taken. The maintainer merges a big architecture
  change, such as a new module or a redesign of how modules fit together, even with every check green.
- **Fuzzing.** An input that crashed a fuzz target joins its seeds in `tests/fuzz/corpus/<target>/`, with
  a test of the fix.
- **Benchmark budgets.** A change raising an allocation budget in `tests/bench` says why.
- **Documentation.** A step of the roadmap is done only once its guide in `docs/` says what it brought.
  Documentation is Markdown in the repository for now. Nothing in CI compiles its C++ blocks yet: compile
  those you write or change against the real headers, and run what a guide says they do.
- **Language.** Code, comments, docs and commit messages are in English.

Agents working in this repository also read [`AGENTS.md`](../AGENTS.md).
