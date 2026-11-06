# Runtimes

Quoll's core does no I/O and has no loop of its own. An adapter brings those: where tasks run, and the
network, the timer and TLS they use. A task asks the executor it runs on for what it needs, so that
`http::server`, `http::client` and `async::sleep_for` take none of it as arguments.

| Adapter             | Where tasks run               | Network | Timer | TLS             | Builds on                         |
| ------------------- | ----------------------------- | ------- | ----- | --------------- | --------------------------------- |
| `quoll::asio`       | An I/O loop, on one thread    | Yes     | Yes   | With OpenSSL    | Native                            |
| `quoll::threads`    | A fixed set of worker threads | No      | No    | No              | Native                            |
| `quoll::emscripten` | The JavaScript event loop     | No      | Yes   | `fetch` does it | Emscripten: browser, Worker, Node |

An operation that needs what its executor does not offer says so: `async::sleep_for`,
`async::with_timeout`, `io::listen` and `io::connect` throw `std::logic_error`. The timeouts in the options
of the server and of the client are different: on an executor without a timer, they do not apply.

The fragments here assume:

```cpp
namespace async = quoll::async;
namespace http = quoll::http;
namespace io = quoll::io;
namespace rest = quoll::rest;
namespace threads = quoll::threads;
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

The CMake variable `QUOLL_RUNTIME` chooses the adapter, and `asio`, the default, is its only value
today. Under Emscripten the target does not exist: a browser runs its own loop.

| Function           | What it does                                                                                                                                                                  |
| ------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `get_executor()`   | An executor of the runtime, to `co_spawn` its tasks on. Copies keep the loop running: hand it to `co_spawn` directly rather than keeping one in `main()`.                     |
| `run()`            | Runs the loop in the calling thread, until no executor of the runtime is left and the work they started has drained.                                                          |
| `get_stop_token()` | A token that the first SIGINT or SIGTERM stops. The signals then get their former actions back, and those the process ignores stay ignored. One runtime at a time takes them. |
| `context()`        | The `asio::io_context`, to mix the runtime with asio code. See below.                                                                                                         |

The runtime can be neither copied nor moved, and it outlives its executors. `run()` and
`get_stop_token()` belong to one thread, and `get_executor()` and the executors may be used from any.

### Blocking work

`async::offload` runs work that blocks away from the loop, on threads of the runtime's own, started by its first use, and
gives what it returns or throws. `http::static_files` reads through it.

```cpp
auto const size = co_await async::offload([&] { return std::filesystem::file_size(path); });
```

On an executor that offers no such thing, as a thread pool and a browser's event loop do not, the work runs right there. A
stop request cancels what has not started, in line behind other work too, and does not interrupt what has. Two threads do this
work, and a call stuck on a dead mount holds one until it returns, and the end of the runtime with it.

## asio

`quoll::asio` is what `quoll::runtime` is natively. A program using only the runtime does not compile
asio. A program naming what follows does, in the same version as the library, as a different one would
not be the same class:

| Name                                                 | What it is                                                                                                        |
| ---------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `runtime.context()`                                  | The `io_context` of the runtime                                                                                   |
| `quoll::asio::executor<>`                            | An executor over any asio executor, an `io_context` or a strand, which is also its network, timer and TLS network |
| `quoll::asio::tcp_stream`, `quoll::asio::tls_stream` | Streams over asio sockets                                                                                         |
| `quoll::asio::connector`                             | A connector opening TCP connections on an asio executor                                                           |
| `quoll::asio::acceptor`, `quoll::asio::tls_acceptor` | Listeners over an asio acceptor                                                                                   |

Quoll fetches asio 1.38.2 unless an `asio::asio` target exists before it. TLS goes through `asio::ssl`
and OpenSSL, as `QUOLL_TLS` says when configuring. Without it, what needs TLS throws `std::logic_error`
at run time.

## The thread pool

`threads::thread_pool` resumes coroutines on a fixed set of worker threads, concurrently. Its
executor offers no network and no timer, so that it serves no HTTP alone, and `sleep_for` throws there.
It holds work for the CPU:

```cpp
threads::thread_pool pool{4};

async::co_spawn(pool.get_executor(), crunch(), async::detached);
```

The pool's destructor lets the workers resume what is queued, then joins them. Every post has to happen
before that starts: a coroutine woken from outside the pool must have been resumed by then. A handler
may also run there, with the accept loop and the sockets on the runtime:

```cpp
auto listener = co_await io::listen(8080);

http::server server{router};

co_await server.serve(pool.get_executor(), std::move(listener));
```

> [!WARNING]
> On such an executor, the timeouts of `connection_options` do not apply, as it has no timer, and a slow
> peer can hold its connection. `shutdown_timeout` then stops requests at once. A handler's own
> `async::with_timeout` throws `std::logic_error`, which answers 500. Handlers run concurrently too, so
> that what they share is theirs to protect. This is an
> [open question](PLAN.md#open-questions) of the roadmap.

`quoll::threads` does not run under WebAssembly: its headers compile there, with nothing to link.

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
target_link_libraries(my_page PRIVATE quoll::emscripten quoll::rest)
target_link_options(my_page PRIVATE -sALLOW_MEMORY_GROWTH)
```

There is no `quoll::runtime` here, and no `run()`. `main()` spawns on the event loop and returns, which
leaves the loop running:

```cpp
#include <quoll/async.hpp>
#include <quoll/client.hpp>
#include <quoll/emscripten.hpp>

#include <iostream>

namespace async = quoll::async;
namespace http = quoll::http;

async::task<> say_hello() {
  http::client client{"https://api.example.com"};
  auto const response = co_await client.get("/hello");

  std::cout << response.body << '\n';
}

int main() {
  async::co_spawn(quoll::emscripten::executor{}, say_hello(), async::detached);
}
```

The same coroutines, clients, contracts and handlers run on both sides, and only `main()` differs:

```cpp
http::client client{"https://api.example.com"};
rest::client<notes::api> notes{"https://api.example.com"};
```

### `http::client` in a browser

A client from a URL asks the executor it runs on how to send, as it asks for a network natively, and the
one here offers the JavaScript `fetch()`. Past a limit, the fetch is aborted, so that memory holds no more than the limit. A URL may also be relative to the page, empty or a path, which only a browser can
make sense of: natively it throws `std::invalid_argument`. On an executor that does not offer `fetch`, a request throws
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
