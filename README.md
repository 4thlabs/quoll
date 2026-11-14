<div align="center">
  <h1>Quoll</h1>

  <p><strong>A C++23 web framework: an async runtime, an HTTP server and client, a typed REST contract, and a UI
  compiled to WebAssembly.</strong><br />
  The same code runs natively and in a browser.</p>

  <p>
    <a href="https://github.com/4thlabs/quoll/actions/workflows/ci.yml"><img src="https://github.com/4thlabs/quoll/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI" /></a>
    <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23" />
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-BSD%203--Clause-blue" alt="BSD 3-Clause" /></a>
  </p>

  <p>
    <a href="docs/getting-started.md">Getting started</a> ·
    <a href="docs/concepts.md">Concepts</a> ·
    <a href="docs/async.md">Async</a> ·
    <a href="docs/server.md">Server</a> ·
    <a href="docs/client.md">Client</a> ·
    <a href="docs/streaming.md">Streaming</a> ·
    <a href="docs/rest.md">REST</a> ·
    <a href="docs/ui.md">UI</a> ·
    <a href="docs/runtimes.md">Runtimes</a> ·
    <a href="docs/design-decisions.md">Design decisions</a>
  </p>
</div>

> [!WARNING]
> **Status: exploration.** The APIs still move and nothing is released yet. What comes next, and in which order, is
> in [`docs/PLAN.md`](docs/PLAN.md).

## What it holds

| Module           | What it gives                                                                                              |
| ---------------- | ---------------------------------------------------------------------------------------------------------- |
| `quoll::async`   | Tasks (`async::task<T>`), `co_spawn`, `when_all`, timeouts, and a stop that ends a task without exception |
| `quoll::io`      | Byte streams, listeners and connectors, TLS, an in-memory stream for tests                               |
| `quoll::http`    | Messages, the parser and serializer, the router, server-sent events and WebSocket frames, all without I/O |
| `quoll::server`  | `http::server`: routes, middlewares, static files, timeouts and limits, a graceful shutdown              |
| `quoll::client`  | `http::client`: kept connections, retries, redirects, TLS, WebSocket                                     |
| `quoll::rest`    | A typed API contract, served with `rest::mount` and called with `rest::client`                           |
| `quoll::ui`      | Components with states and props, callbacks, JavaScript widgets, Tailwind, `to_html`                    |
| `quoll::runtime` | The loop a program runs on: asio natively, the browser's event loop under Emscripten                    |

## Quick start

A `CMakeLists.txt`, with [CPM](https://github.com/cpm-cmake/CPM.cmake) in `cmake/CPM.cmake`:

```cmake
cmake_minimum_required(VERSION 3.23)
project(hello LANGUAGES CXX)

include(cmake/CPM.cmake)
CPMAddPackage("gh:4thlabs/quoll#main")

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE quoll::rest quoll::runtime quoll::server)
```

And a `main.cpp`, holding a contract with one route, its server and a client calling it:

```cpp
#include <quoll/async.hpp>
#include <quoll/http.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server.hpp>

#include <iostream>
#include <string>

namespace async = quoll::async;
namespace http = quoll::http;
namespace rest = quoll::rest;

struct greeting {
  std::string text;
};

// The contract: types and endpoints, shared by the server and the client.
using greet = rest::endpoint<"greet", http::verb::get, "/greet/{name}", rest::returns<greeting>>;
using api = rest::api<"/api", greet>;

// The server: one operator() per endpoint. A missing one does not compile.
struct greeter {
  greeting operator()(greet, std::string name) const { return {"Hello, " + name + "!"}; }
};

async::task<> serve() {
  http::server server;

  rest::mount<api>(server, greeter{});
  co_await server.listen(8080);
}

// The client: the same contract, so that a wrong call does not compile.
async::task<> call() {
  rest::client<api> client{"http://127.0.0.1:8080"};
  auto const answer = co_await client.call<greet>("Ada");

  std::cout << answer.text << '\n';
}

int main(int argc, char **) {
  quoll::runtime runtime;

  // Runs it with a stop token Ctrl-C stops, which shuts the server down; fails the program if it fails.
  return runtime.run(argc > 1 ? call() : serve());
}
```

Build it and run both sides:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/hello &                               # the server, on port 8080
sleep 1                                       # give it time to listen
./build/hello call                            # Hello, Ada!
curl localhost:8080/api/greet/Ada             # {"text":"Hello, Ada!"}
kill %1
```

A larger example, with its own tests, is in [`samples/notes`](samples/notes). The whole walkthrough
is in [Getting started](docs/getting-started.md).

## How it is tested

Every pull request and push to `main` goes through:

- **Builds and tests** on GCC 13, Clang 19, Apple Clang (macOS) and Emscripten, whose tests run under Node.
- **Sanitizers:** the whole test suite under ThreadSanitizer, and under AddressSanitizer with UndefinedBehaviorSanitizer.
- **What must not compile:** `compile_fail` cases, one per check of an API, each passing only when the compiler
  refuses it with the expected message.
- **What a debug build asserts:** `assert` cases, which break an invariant out of a program's reach and pass only when
  the program ends on the assertion.
- **Self-contained headers:** each one compiled alone, in its own translation unit.
- **clang-tidy**, on what it finds real bugs with (use after move, dangling handles, the analyzer's move and
  new/delete checks), as errors; `clang-format`.
- **Reviews:** a review of the whole diff before each push, and another by Codex on the pull request, every point of
  which is answered before it merges; C++ that borrows is checked by hand for lifetimes, as Rust's borrow checker
  would.

Every night, and on a pull request touching their targets:

- **Fuzzing**, with libFuzzer under AddressSanitizer and UndefinedBehaviorSanitizer: the request, response,
  server-sent event and WebSocket parsers, the router, a connection served from arbitrary bytes, the client and the
  REST server and client, including streams that deliver their bytes one at a time. An input that crashed a target
  joins its seeds, with a test of the fix.
- **Load**, under ThreadSanitizer, then under AddressSanitizer with UndefinedBehaviorSanitizer: a server and its
  clients on runtimes of several threads, with well-formed requests, stops, peers gone in mid-request, requests past
  the limits and stalling peers. A sanitizer's report or a wrong answer fails it.
- **Interoperability**, against third-party peers in containers: curl, Python's requests, httpx, http.client and
  websockets, Node's fetch and WebSocket as clients; aiohttp's and Node's servers; nginx as a proxy; and Autobahn's
  WebSocket suite, each way.
- **Benchmarks**, which also check the allocations per request against fixed budgets: see
  [Performance](docs/performance.md).
- **clang-tidy with its static analyzer** over the tests too.

## Documentation

| Guide                                          | What it covers                                                                                    |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| [Getting started](docs/getting-started.md)     | Adding Quoll with CMake, a first server and client, how a program runs                            |
| [Concepts](docs/concepts.md)                   | How Quoll works: sans-IO, runtimes and adapters, tasks and stops, what WebAssembly asks           |
| [Async](docs/async.md)                         | Tasks, running and combining them, timeouts, stopping a task                                      |
| [Server](docs/server.md)                       | Routes, handlers, middlewares, `listen` and `serve`, timeouts, shutdown, errors, on the Internet |
| [Client](docs/client.md)                       | `http::client`, URLs, kept connections and retries, redirects, TLS, failures                      |
| [Streaming](docs/streaming.md)                 | Bodies in pieces on both sides, server-sent events, WebSocket                                     |
| [REST](docs/rest.md)                           | The contract, `mount`, `rest::client`, codecs, third-party APIs                                  |
| [Command line](docs/command-line.md)           | A command-line program calling an API, natively and in Node                                       |
| [UI](docs/ui.md)                               | Components, props, callbacks, views not run again, JavaScript widgets, `to_html`, the build      |
| [UI life cycle](docs/ui-lifecycle.md)          | How a component is made, viewed, runs its effects and goes, in diagrams                          |
| [Runtimes](docs/runtimes.md)                   | `quoll::runtime`, the adapters, `http::client` in a browser, what WebAssembly asks of you         |
| [Performance](docs/performance.md)             | What Quoll costs, and how it is measured                                                          |
| [Design decisions](docs/design-decisions.md)   | The choices Quoll made where they matter to a user, the options set aside, and what follows       |
| [Development](docs/development.md)             | Building from source, the tests, the checks of CI, how to contribute                              |

## Requirements

- **To build:** C++23, with GCC 13, Clang 19 or Apple Clang 21 at least, and CMake 3.23.
- **In a browser:** Chrome 112, Edge 112, Firefox 121 or Safari 18.2 at least, all there since
  December 2024, for WebAssembly tail calls. Without them, a coroutine resuming another grows the
  stack, and a long loop of tasks that never suspend overflows it. An older browser does not load
  the module, and the page stays empty.

## Built with

[llhttp](https://github.com/nodejs/llhttp) for HTTP parsing ·
[Glaze](https://github.com/stephenberry/glaze) for JSON and reflection ·
[asio](https://github.com/chriskohlhoff/asio) for the native runtime ·
[OpenSSL](https://www.openssl.org) for TLS ·
[Catch2](https://github.com/catchorg/Catch2) for the tests.

## License

BSD 3-Clause, see [`LICENSE.md`](LICENSE.md).
