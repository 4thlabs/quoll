# Getting started

Quoll is a set of compiled libraries with templates in headers, built with CMake. A program links
the modules it uses, and a coroutine does the rest: nothing else has to be set up.

## Requirements

- GCC 13, Clang 19 or Apple Clang 21 at least, in C++23, and CMake 3.23.
- OpenSSL, for TLS. Without it Quoll still builds, and what needs TLS fails at run time, saying so.
- To run in a browser: see [the requirements](../README.md#requirements) and
  [Runtimes](runtimes.md#emscripten).

## Adding Quoll

### With CPM

[CPM](https://github.com/cpm-cmake/CPM.cmake) fetches Quoll and builds it with your program, along
with what it needs (llhttp, Glaze, asio). Put `CPM.cmake` in your project:

```bash
mkdir -p cmake
curl -fsSL -o cmake/CPM.cmake https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/CPM.cmake
```

Then:

```cmake
cmake_minimum_required(VERSION 3.23)
project(my_app LANGUAGES CXX)

include(cmake/CPM.cmake)
CPMAddPackage("gh:4thlabs/quoll#main")

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE quoll::runtime quoll::server)
```

Quoll has no release yet: pin the commit you tested in place of `main`.

Added this way, Quoll builds neither its tests nor its samples, and writes no install rules. Its
options go through `OPTIONS`:

```cmake
CPMAddPackage(
  NAME quoll
  GITHUB_REPOSITORY 4thlabs/quoll
  GIT_TAG main
  OPTIONS "QUOLL_TLS ON"
)
```

| Option          | Default | What it does                                                                         |
| --------------- | ------- | ------------------------------------------------------------------------------------ |
| `QUOLL_TLS`     | `AUTO`  | TLS through OpenSSL: `AUTO` uses it when found, `ON` requires it, `OFF` goes without |
| `QUOLL_RUNTIME` | per platform | The adapter behind `quoll::runtime`: see [Runtimes](runtimes.md)                |

If your program already has an `asio::asio` or a `glaze::glaze` target, define it before adding
Quoll, which then uses it rather than fetching its own copy.

### With `find_package`

Build and install Quoll once:

```bash
git clone https://github.com/4thlabs/quoll.git && cd quoll
cmake -B build -DCMAKE_BUILD_TYPE=Release -DQUOLL_BUILD_TESTS=OFF -DQUOLL_BUILD_SAMPLES=OFF
cmake --build build -j"$(nproc)"
cmake --install build --prefix /opt/quoll
```

Each module is a component of the package:

```cmake
find_package(quoll REQUIRED COMPONENTS runtime server)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE quoll::runtime quoll::server)
```

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/opt/quoll
```

A program using `quoll::runtime` compiles no asio, and needs none installed. An installed Quoll
built with TLS does need OpenSSL to be found.

`quoll::rest` and `quoll::ui` need [Glaze](https://github.com/stephenberry/glaze) in your program,
in the version Quoll was built with (9.1.0). Either `find_package(glaze)` finds it, or you define
the target before finding Quoll. Glaze needs nothing but its headers:

```cmake
include(cmake/CPM.cmake)
CPMAddPackage(
  NAME glaze
  VERSION 9.1.0
  URL https://github.com/stephenberry/glaze/archive/refs/tags/v9.1.0.tar.gz
  DOWNLOAD_ONLY YES
)
add_library(glaze_headers INTERFACE)
target_include_directories(glaze_headers SYSTEM INTERFACE ${glaze_SOURCE_DIR}/include)
add_library(glaze::glaze ALIAS glaze_headers)

find_package(quoll REQUIRED COMPONENTS rest runtime server)
```

> [!IMPORTANT]
> Build your program in the same mode as its Quoll, as `Debug` or as `Release`. An `assert` in a
> source follows the library's build type, and inline code holding one, compiled on both sides, is
> an ODR violation when the modes differ, which nothing detects. With CPM, this is taken care of.

### The modules

| Target                                               | What it brings                                                                        |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------- |
| `quoll::async`                                       | `task`, `co_spawn`, `when_all`, `sleep_for`, `with_timeout`                           |
| `quoll::io`                                          | Byte streams, listeners, connectors, `io::connect`, `io::listen`, an in-memory stream |
| `quoll::http`                                        | Messages, the parser and serializer, the router, `http::local_transport`              |
| `quoll::server`                                      | `http::server`, `http::static_files`, `http::connection_options`                      |
| `quoll::client`                                      | `http::client`                                                                        |
| `quoll::rest`                                        | Contracts, `rest::mount`, `rest::client`, codecs                                      |
| `quoll::ui`                                          | The [UI prototype](ui.md)                                                             |
| `quoll::runtime`                                     | `quoll::runtime`, the loop a program runs on: see [Runtimes](runtimes.md)      |
| `quoll::asio`, `quoll::emscripten`                  | The adapters, which a program rarely names: see [Runtimes](runtimes.md)               |

`quoll::server` and `quoll::client` are libraries of their own, so that a program links only what it uses, and
their names are in the namespace `quoll::http`.

A target brings the modules it depends on: linking `quoll::server` brings `quoll::http`, which
brings `quoll::io` and `quoll::async`. Each module has an umbrella header, such as
`<quoll/http.hpp>`, and one header per component, such as `<quoll/http/router.hpp>`.

## A first server

Link `quoll::runtime` and `quoll::server`. This `main.cpp` answers `GET /hello/{name}`:

```cpp
#include <quoll/async.hpp>
#include <quoll/http.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server.hpp>

#include <string>

namespace async = quoll::async;
namespace http = quoll::http;

async::task<> serve_hello() {
  http::server server;

  server.get("/hello/{name}", [](std::string name) {
    http::response response;

    response.body = "Hello, " + name + "!";
    return response;
  });
  co_await server.listen(8080);
}

int main() {
  quoll::runtime runtime;

  async::co_spawn(runtime.get_executor(), serve_hello(), async::detached, runtime.get_stop_token());
  runtime.run();
}
```

```bash
curl localhost:8080/hello/Ada     # Hello, Ada!
```

It listens on the loopback interface until Ctrl-C. Routes, middlewares, options and everything else
of a server are in [Server](server.md).

## A first client

Link `quoll::runtime` and `quoll::client`. This one calls the server above:

```cpp
#include <quoll/async.hpp>
#include <quoll/client.hpp>
#include <quoll/http.hpp>
#include <quoll/runtime.hpp>

#include <iostream>

namespace async = quoll::async;
namespace http = quoll::http;

async::task<> say_hello() {
  http::client client{"http://127.0.0.1:8080"};
  auto const response = co_await client.get("/hello/Ada");

  std::cout << static_cast<int>(response.status) << ' ' << response.body << '\n';
}

int main() {
  quoll::runtime runtime;

  async::co_spawn(runtime.get_executor(), say_hello(), async::detached);
  runtime.run();
}
```

Every response is a result, a 404 included. What fails is getting one: see [Client](client.md).

If you are writing both sides of an API, or calling one with typed requests and responses, use a
[REST contract](rest.md) rather than routes and requests by hand, as in the
[quick start](../README.md#quick-start).

## How a program runs

`main()` creates the runtime, starts the program's main coroutine on it, and runs it:

- **`quoll::runtime`** is the loop a program's tasks run on, run by the thread that calls `run()`.
  It holds the network, the timer and TLS, which is why `http::server` and `http::client` ask for none of
  them. Natively it is asio underneath, and your program does not compile asio. In a browser, it is the page's own loop, and `main()` reads the
  same, with the differences [its guide](runtimes.md#emscripten) says.
- **`async::task<T>`** is a coroutine producing a `T`, or nothing for `async::task<>`. It is lazy: it starts
  when awaited, and destroying one that never ran does not run it. An exception it lets out is
  thrown again at its `co_await`.
- **`async::co_spawn`** runs a task from outside a coroutine, from the executor and never inline, and
  gives what comes of it to a handler. With `async::detached`, a value is dropped, and an exception
  ends the program, as one escaping a `std::thread` does, unless it is `std::errc::operation_canceled`, what a stop
  request throws.
- **`runtime.run()`** returns once the tasks have ended. An executor kept in `main()` would keep it
  running, so give `get_executor()` straight to `co_spawn`.
- **`runtime.get_stop_token()`** is stopped by the first SIGINT or SIGTERM. Given to `co_spawn`, it
  is what shuts a server down. The signals then get their former actions back, so that a second
  Ctrl-C ends a process whose tasks do not stop.

What does I/O, a server or a client with its kept connections, lives in the tasks, not in `main()`:
in the sample and the guides, they are local variables of the main coroutine.

### Doing several things

```cpp
using namespace std::chrono_literals;

// One after the other.
auto const first = co_await load_user(1);

// At once: both run, and it completes once they have.
auto [second, third] = co_await async::when_all(load_user(2), load_user(3));

// With a deadline: the task is stopped, and awaited to its end.
auto const fourth = co_await async::with_timeout(2s, load_user(4));

co_await async::sleep_for(100ms);
```

`with_timeout` asks the task to stop and waits for it to end, so that nothing it refers to is left
behind. A task that does not look at its stop token runs to its end, and then gives its result.
Quoll's own operations look at it: a sleep, a connection, a read or a write. When a timeout expires,
the task's `operation_canceled` becomes a `std::system_error` with `std::errc::timed_out`.

A task that runs on its own, while the coroutine goes on:

```cpp
co_await async::co_spawn(refresh_cache(), async::detached);
```

This does not wait for the task, and does not suspend. The task does not take the stop token of the
coroutine: pass it `co_await async::current_stop_token()` for it to stop along with it, and see
[below](#failures) for what a stop does to it.

### Failures

With `async::detached`, what a task lets out ends the program, which suits a program that has nothing
to add to it. To carry on, catch it in the coroutine:

```cpp
try {
  auto const response = co_await client.get("/hello");

  std::cout << response.body << '\n';
} catch (std::system_error const &failure) {
  // A stop request ends the task, rather than being one more failure to carry on from.
  if (failure.code() == std::errc::operation_canceled) {
    throw;
  }

  std::cerr << "no answer: " << failure.what() << '\n';
}
```

A stop request is no failure: what the task awaits throws `std::system_error` with
`std::errc::operation_canceled`, and that ends the task, with `async::detached` as well, and the program
goes on. Catch it only to do something before the task ends, and let it out again. `async::detached`
ignores that error whatever caused it, a closed socket or a read after a cancelled one too: a task that
cares can check `co_await async::current_stop_token()` to tell them apart.

## Where to go next

- [Server](server.md) and [Client](client.md), for HTTP by hand.
- [REST](rest.md), for an API described once.
- [Streaming](streaming.md), for bodies that do not fit in memory.
- [`samples/notes`](../samples/notes): a REST contract, its server and its client over TCP.
