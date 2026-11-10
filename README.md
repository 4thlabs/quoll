<div align="center">
  <h1>Quoll</h1>

  <p><strong>A C++23 web framework built for performance and ease of use.</strong><br />
  A sans-IO async runtime, an HTTP server and client, and a typed REST contract shared by both, with the same code natively and in WebAssembly.</p>

  <p>
    <a href="https://github.com/4thlabs/quoll/actions/workflows/ci.yml"><img src="https://github.com/4thlabs/quoll/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI" /></a>
    <img src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white" alt="C++23" />
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-BSD%203--Clause-blue" alt="BSD 3-Clause" /></a>
  </p>

  <p>
    <a href="docs/getting-started.md">Getting started</a> ·
    <a href="docs/server.md">Server</a> ·
    <a href="docs/client.md">Client</a> ·
    <a href="docs/streaming.md">Streaming</a> ·
    <a href="docs/rest.md">REST</a> ·
    <a href="docs/command-line.md">Command line</a> ·
    <a href="docs/runtimes.md">Runtimes</a> ·
    <a href="docs/ui.md">UI</a> ·
    <a href="docs/performance.md">Performance</a> ·
    <a href="docs/development.md">Development</a>
  </p>
</div>

> [!WARNING]
> **Status: exploration.** The APIs are still moving and nothing is released yet. The target and
> the order of the work are in [`docs/PLAN.md`](docs/PLAN.md).

## Why Quoll

Writing a web service in C++ tends to mean choosing between what is fast and what is pleasant to
use, and writing the same API twice, once to serve it and once to call it. Quoll aims at both, with
one description of an API:

- **Fast, and measured.** One thread answers about 148,000 small requests a second over 16
  connections, on the machine they were measured on. A benchmark runs every night and holds the
  allocations per request to a budget: see [Performance](docs/performance.md).
- **Easy to use.** A program is a coroutine. `co_await server.listen(8080)` is a
  server, and `rest::client<api> client{"http://127.0.0.1:8080"}` is a client. Neither asks for an
  executor, a connector or an acceptor.
- **Sans-IO.** The HTTP parser, the serializer and the router do no I/O, and a connection is served
  over any byte stream. TCP, TLS and the memory stream of a test are the same code to a server,
  and an adapter brings the network, the timer and TLS: see [Runtimes](docs/runtimes.md).
- **The same code natively and in WebAssembly.** The core needs no threads and no blocking waits.
  A REST client goes over TCP natively and over `fetch` in a browser, from the same contract.
- **One typed contract.** An API is a set of C++ types, shared by the server and the client. A call
  with the wrong arguments or a service missing an endpoint does not compile, and a contract for
  someone else's API needs no Quoll server: see [REST](docs/rest.md).
- **Safe defaults.** Timeouts and size limits are on by default, a stop request shuts down
  gracefully, and `https://` checks the server's certificate unless told otherwise. See
  [Server](docs/server.md) and [Client](docs/client.md).
- **Bodies larger than memory.** Requests and responses can be read and written in pieces, on both
  sides: see [Streaming](docs/streaming.md).

A UI written in C++ and compiled to WebAssembly is there too, as a prototype: see [UI](docs/ui.md).

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

## Documentation

| Guide                                      | What it covers                                                                  |
| ------------------------------------------ | ------------------------------------------------------------------------------- |
| [Getting started](docs/getting-started.md) | Adding Quoll with CMake, a first server and client, how a program runs          |
| [Server](docs/server.md)                   | Routes, handlers, middlewares, `listen` and `serve`, timeouts, shutdown, errors, on the Internet |
| [Client](docs/client.md)                   | `http::client`, URLs, keep-alive and retries, redirects, TLS, failures          |
| [Streaming](docs/streaming.md)             | Reading and writing bodies in pieces, on both sides, and what ends them         |
| [REST](docs/rest.md)                       | The contract, `mount`, `rest::client`, codecs, third-party APIs                 |
| [Runtimes](docs/runtimes.md)               | `quoll::runtime`, the adapters, `http::client` in a browser, what WebAssembly asks of you    |
| [UI](docs/ui.md)                           | The `quoll::ui` prototype                                                       |
| [Performance](docs/performance.md)         | What Quoll costs, and how it is measured                                        |
| [Development](docs/development.md)         | Building from source, tests, the checks of CI, how to contribute                |

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
