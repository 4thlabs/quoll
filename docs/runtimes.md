# Runtimes

Quoll's core does no I/O and has no loop of its own. An adapter brings those: where tasks run, and the
network, the timer and TLS they use. A task asks the executor it runs on for what it needs, so that
`http::server`, `http::client` and `async::sleep_for` take none of it as arguments.

| Adapter             | Where tasks run                    | Network | Timer | TLS             | Builds on                         |
| ------------------- | ---------------------------------- | ------- | ----- | --------------- | --------------------------------- |
| `quoll::asio`       | An I/O loop, on one thread or more | Yes     | Yes   | With OpenSSL    | Native                            |
| `quoll::emscripten` | The JavaScript event loop          | No      | Yes   | `fetch` does it | Emscripten: browser, Worker, Node |

An operation that needs what its executor does not offer says so: `async::sleep_for`,
`async::with_timeout`, `io::listen` and `io::connect` throw `std::logic_error`. The timeouts in the options
of the server and of the client are different: on an executor without a timer, they do not apply.

The fragments here assume:

```cpp
namespace async = quoll::async;
namespace http = quoll::http;
namespace io = quoll::io;
namespace rest = quoll::rest;
```

`quoll::asio` and `quoll::emscripten` keep their full names, as `asio::` and `quoll::emscripten::` are what they adapt.

## `quoll::runtime`

`quoll::runtime`, from `<quoll/runtime.hpp>` and the target `quoll::runtime`, is the runtime of the
build. Application code names no adapter:

```cpp
#include <quoll/async.hpp>
#include <quoll/runtime.hpp>

#include <chrono>
#include <iostream>

namespace async = quoll::async;

async::task<> tick() {
  for (auto left = 3; left > 0; --left) {
    std::cout << left << std::endl;
    co_await async::sleep_for(std::chrono::seconds{1});
  }
}

int main() {
  quoll::runtime runtime;

  async::co_spawn(runtime.get_executor(), tick(), async::detached, runtime.get_stop_token());
  runtime.run();
}
```

The CMake variable `QUOLL_RUNTIME` chooses the adapter, and has one value per platform today: `asio` natively, and `emscripten`
under `emcmake`, whose differences are [below](#emscripten). This table is the native one.

| Function           | What it does                                                                                                                                                                  |
| ------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `get_executor()`   | An executor of the runtime, to `co_spawn` its tasks on. Copies keep the loop running: hand it to `co_spawn` directly rather than keeping one in `main()`.                     |
| `run()`            | Runs the loop in the calling thread, until no executor of the runtime is left and the work they started has drained.                                                          |
| `get_stop_token()` | A token that the first SIGINT or SIGTERM stops. The signals then get their former actions back, and those the process ignores stay ignored. One runtime at a time takes them. |
| `context()`        | The `asio::io_context`, to mix the runtime with asio code. See below.                                                                                                         |

The runtime can be neither copied nor moved, and it outlives its executors. `run()` and
`get_stop_token()` belong to one thread, outside `run()` for `get_stop_token()`, never from a task; `get_executor()` and
the executors may be used from any.

### Several threads

A runtime runs one thread by default, the one calling `run()`, and a program's tasks take turns on it, with nothing to pay
for more. Its options ask for more, the same on every runtime, so that `main()` does not change with the build: a browser
runs one whatever they say.

```cpp
quoll::runtime runtime{quoll::async::runtime_options{.threads = 4}};
```

`run()` then runs the loop on four threads, its own included. Each executor `get_executor()` hands out is a **strand**: the
tasks spawned on it, and the tasks they await, take turns, whichever thread runs them, while those on other executors run
in parallel. A server runs each connection on a strand of its own, so that what a connection does beside itself (reading
while it writes, a timer, a stop) never runs on two threads at once, and each socket, acceptor and timer is on its own too,
as asio asks.

What two tasks on different executors share is the program's to protect: a `std::atomic`, a mutex, or, for tasks of its
own, one executor for both. A handler's state shared between connections is such a thing, and takes an atomic or a mutex,
as each connection has a strand of its own.

### Blocking work

`async::offload` runs work that blocks away from the loop, on threads of the runtime's own, started by its first use, and
gives what it returns or throws. `http::static_files` reads through it.

```cpp
auto const size = co_await async::offload([&] { return std::filesystem::file_size(path); });
```

On an executor that offers no such thing, as `async::run_loop` and a browser's event loop do not, the work runs right there. A
stop request cancels what has not started, in line behind other work too, and does not interrupt what has. Two threads do this
work, and a call stuck on a dead mount holds one until it returns, and the end of the runtime with it.

## asio

`quoll::asio` is what `quoll::runtime` is natively. A program using only the runtime does not compile
asio. A program naming what follows does, in the same version as the library, as a different one would
not be the same class:

| Name                                                 | What it is                                                                                                        |
| ---------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `runtime.context()`                                  | The `io_context` of the runtime                                                                                   |
| `quoll::asio::executor<>`                            | An executor over any asio executor, an `io_context` or a strand, which is also its network, timer and TLS network. Its I/O objects need one handler at a time: with `object_strands::own`, each is on a strand of its own, for an `io_context` run by several threads |
| `quoll::asio::tcp_stream`, `quoll::asio::tls_stream` | Streams over asio sockets                                                                                         |
| `quoll::asio::connector`                             | A connector opening TCP connections on an asio executor                                                           |
| `quoll::asio::acceptor`, `quoll::asio::tls_acceptor` | Listeners over an asio acceptor                                                                                   |

Quoll fetches asio 1.38.2 unless an `asio::asio` target exists before it. TLS goes through `asio::ssl`
and OpenSSL, as `QUOLL_TLS` says when configuring. Without it, what needs TLS throws `std::logic_error`
at run time.

## Emscripten

`quoll::emscripten` runs tasks on the JavaScript event loop, in a browser, a Worker or Node. Build with
`emcmake` and link it, which brings its JavaScript side (`--js-library`) and the flags Quoll needs,
`-fwasm-exceptions` and `-mtail-call`:

```bash
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm -j"$(nproc)"
```

```cmake
add_executable(my_page main.cpp)
target_link_libraries(my_page PRIVATE quoll::runtime quoll::rest)
target_link_options(my_page PRIVATE -sALLOW_MEMORY_GROWTH)
```

`quoll::runtime` is the page's event loop here, and `main()` reads as it does natively. The loop is the
browser's and goes on once `main()` has returned, so that `run()` returns at once, before any task has run, and what
follows it runs first. Its stop token never stops, as a page has no Ctrl-C.

> [!WARNING]
> A task does not refer to a local of `main()`, which is valid natively, as `run()` waits, and is gone here when the
> task runs. For the same code to run on both, what a task uses lives in it.

```cpp
#include <quoll/async.hpp>
#include <quoll/client.hpp>
#include <quoll/runtime.hpp>

#include <iostream>

namespace async = quoll::async;
namespace http = quoll::http;

async::task<> say_hello() {
  http::client client{"https://api.example.com"};
  auto const response = co_await client.get("/hello");

  std::cout << response.body << '\n';
}

int main() {
  quoll::runtime runtime;

  async::co_spawn(runtime.get_executor(), say_hello(), async::detached, runtime.get_stop_token());
  runtime.run();
}
```

The same `main()`, coroutines, clients and contracts run on both sides, but for what a browser cannot do: a server, there, throws `std::logic_error` when it listens, as the executor offers no network.

```cpp
http::client client{"https://api.example.com"};
rest::client<notes::api> notes{"https://api.example.com"};
```

### `http::client` in a browser

A client from a URL asks the executor it runs on how to send, as it asks for a network natively, and the
one here offers the JavaScript `fetch()`. Past a limit, the fetch is aborted, so that memory holds no more than the limit. A URL may also be relative, which only a browser can make sense of, and natively
it throws `std::invalid_argument`: empty or a path, from the page's origin, or `.` or `./path`, from the page itself, for one served
under a path of its own. What `fetch` refuses throws `std::invalid_argument` before the task runs: a GET or a HEAD with a body or a
`body_writer`, and the methods CONNECT and TRACE. On an executor that does not offer `fetch`, a request throws
`std::logic_error`, as one without a network does natively. A connector given to a client is still used, as a test in memory does.

What `fetch` does for you, and what it does not let you do:

| Point                | Natively                                      | In a browser                                                                                                                                      |
| -------------------- | --------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Streaming a response | [`stream()`](streaming.md#reading-a-response) | [`stream()`](streaming.md#in-a-browser)                                                                    |
| Streaming a request  | [In pieces](streaming.md#writing-a-request)   | A `body_writer` runs to its end before the fetch, and goes out whole                                                                               |
| Redirects            | Followed if asked, to its own server only     | Always followed, possibly to another origin. The 3xx response is not exposed.                                                                      |
| Fields sent          | All of them                                   | Not those `fetch` manages itself (`Host`, `Content-Length`, `Connection` and the other hop-by-hop fields), nor the trailers.                       |
| Fields received      | As received                                   | In lower case, repeated ones combined, without those describing the bytes on the wire (`Content-Encoding`, `Content-Length`, `Transfer-Encoding`). |
| Body received        | As received                                   | Decoded: decompressed and de-chunked.                                                                                                              |
| HTTP version         | As received                                   | Not known: `http_1_1`.                                                                                                                             |
| Options              | [`client_options`](client.md#options)         | `limits` and `response_timeout` apply. `max_connections`, `max_idle_connections`, `max_redirects`, `connect_timeout` and `tls` have no effect, as the browser decides, and it always checks certificates.                                                      |

A failure to get a response (a network error, a refused connection, CORS) throws `std::system_error`
with `std::errc::io_error` and `fetch`'s message. A stop request aborts the fetch, with
`std::errc::operation_canceled`. A target not in origin form, and what `http::serialize` rejects, throw
`std::invalid_argument` before the task runs.

Use a client, and request a stop, from the thread running the event loop.

### What WebAssembly asks of you

- **No sockets.** No server in a browser, and no `io::connect` or `io::listen`: they throw
  `std::logic_error` there, as the executor offers no network. Outgoing HTTP goes through `fetch`.
- **No threads, and no blocking.** The core needs neither. On the event loop, a task that does not
  suspend holds up the page: nothing renders and no `fetch` completes meanwhile. A coroutine that yields
  endlessly starves the loop, as an endless chain of promises would.
- **Timers through `setTimeout`.** A wait lasts at most what `setTimeout` can, about 24.8 days.
- **Modern browsers.** Chrome 112, Edge 112, Firefox 121 or Safari 18.2 at least, for WebAssembly tail
  calls. Without them, a coroutine resuming another grows the stack, and a long loop of tasks that never
  suspend overflows it. An older browser does not load the module, and the page stays empty.

For an interface in the page, see [UI](ui.md).
