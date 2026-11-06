# Server

A server is a router, which maps requests to handlers and does no I/O, and something feeding it the
requests of connections: `server::listen` most of the time. It speaks HTTP/1.x, over TCP or TLS.
Link `quoll::runtime` and `quoll::server`.

To serve a typed API rather than routes by hand, see [REST](rest.md), which registers its routes in
this same router. The examples here assume:

```cpp
namespace http = quoll::http;
using quoll::async::task;
using namespace std::chrono_literals;
```

## Routes

`router.get`, `post`, `put`, `patch`, `delete_`, `head` and `options` take a path and a handler. The
handler takes the parameters of the path, by position, and may take the request first:

```cpp
router.get("/users/{id}", [](std::int64_t id) {
  http::response response;

  response.body = "user " + std::to_string(id);
  return response;
});

router.post("/users/{id}/cards", [](http::request request, std::int64_t id) -> task<http::response> {
  co_await add_card(id, request.body);

  http::response response;

  response.status = http::status::created;
  co_return response;
});

router.get("/files/{rest:path}", [](std::string rest) {
  http::response response;

  response.body = std::move(rest);
  return response;
});
```

The handler says what the parameters are: a number, a `bool` (`true` or `false`), a `std::string` or
a `std::string_view`. A value that cannot be read as its type answers 400. The handler returns
`http::response`, or `task<http::response>` when it has something to await.

All of this is checked at compile time: the path is a string literal and a valid template, with as
many parameters as the handler takes, which has types (no `auto`). A path made at run time goes
through `add()`, which also hands over the name of the route and its query:

```cpp
router.add(http::verb::get, "/search", "search",
           [](http::request, http::route_match match) -> task<http::response> {
             http::response response;

             // After `?`, still encoded.
             response.body = std::move(match.query);
             co_return response;
           });
```

References and views a handler takes stay valid in a coroutine too, until its task ends.

### How a request finds its route

- A parameter takes a whole segment. `{name:path}` takes the rest of the path, and comes last.
- The most specific route wins, segment by segment: a literal beats a parameter, which beats the
  rest of the path, so that `/users/me` wins over `/users/{id}`.
- Segments are compared after percent-decoding. `/users` and `/users/` are different paths.
- A HEAD request without a HEAD route goes to the GET route, and its response goes out without the
  body, announcing its length.

| What comes                                                                | Answer, with no body |
| ------------------------------------------------------------------------- | -------------------- |
| A path that no route matches                                              | 404                  |
| A path whose routes have other methods                                    | 405, with `Allow`    |
| Another form of target, an invalid escape, a NUL or a `.` or `..` segment | 400                  |

`add()` and the functions above throw `std::invalid_argument` for an invalid template, an empty name
or handler, or a route this method already has with the same shape.

Add routes and middlewares first, then serve: from then on, `dispatch()` may be called from any
thread. The router outlives the tasks that serve it.

## Responses

`http::response` is an aggregate: `status`, `fields`, `body`, `trailers`, and `body_writer` to write
the body in pieces (see [Streaming](streaming.md)). `Content-Length` is added when the response has a
body and its fields declare no framing. Quoll adds no `Date` or `Server` field.

```cpp
router.get("/hello", [](http::request) {
  http::response response;

  response.fields.set("Content-Type", "text/plain");
  response.body = "hello";
  return response;
});
```

A handler setting `Connection: close` ends the connection after its response.

## Middlewares

`router.use` runs a function around every request. It answers by awaiting `next`, changing what it
likes on the way, or by itself. The first one added is the outermost.

```cpp
router.use([](http::request request, http::next const &next) -> task<http::response> {
  if (next.path().starts_with("/admin") && !request.fields.contains("Authorization")) {
    http::response refused;

    refused.status = http::status::unauthorized;
    co_return refused;
  }

  auto response = co_await next(std::move(request));

  response.fields.set("X-Served-By", "quoll");
  co_return response;
});
```

- Routing is done by then. `next.path()`, `next.route()` and `next.params()` say what it found, and
  are what to take a security decision on: `request.target` is as the peer sent it, and `/%61dmin` is
  routed to `/admin`.
- Middlewares wrap the router's own 404, 405 and 400, as logs, CORS or a rate limit need them. A
  request refused as it is read (400, 413...) reaches none of them.
- What it does after `next` does not run for what `next` throws. Catch it to answer a 500 of its own,
  or the connection answers one without it.
- What it changes in the request reaches the handler, but not what comes of its method and target:
  the route, its parameters and its query are those that came.

## Serving

`server::listen` listens on a port and serves until a stop request. It listens through the network of
the executor it runs on, so that it needs nothing else. It takes the loopback interface by default:
ask for `0.0.0.0` or `::` to be reachable from outside.

```cpp
co_await quoll::server::listen(router, 8080, {.address = "0.0.0.0"});
```

It throws `std::system_error` when the address cannot be bound or the certificate loaded.

To know the port before serving, or that the server is ready, listen first and then `serve`. With
port 0, the system picks a free one:

```cpp
auto listener = co_await quoll::io::listen(0);

std::cout << "listening on " << listener.local_port().value_or(0) << std::endl;
co_await quoll::server::serve(listener, router);
```

`serve` takes anything modelling `io::Listener`. Each connection runs as a task of its own, so that
connections are served concurrently, one request at a time on each.

`serve_connection` serves one connection, over anything modelling `io::ByteStream`, until it ends.
This is how a test runs a server over a memory stream:

```cpp
auto [client_side, server_side] = quoll::io::make_memory_stream_pair();

co_await quoll::server::serve_connection(std::move(server_side), router);
```

In all three, `router` outlives the task.

> [!NOTE]
> `quoll::runtime` runs everything on the thread that calls `run()`. A handler that blocks that
> thread holds up every connection: what takes time is awaited, not waited for.

## Options

`listen` takes `listen_options`, holding `serve_options`, holding `connection_options`:

```cpp
co_await quoll::server::listen(
    router, 8080,
    {.address = "0.0.0.0",
     .serve = {.connection = {.idle_timeout = 30s,
                              .read_timeout = 10s,
                              .on_handler_error = [](std::exception_ptr failure) { report(failure); }},
               .max_connections = 1000}});
```

### `connection_options`

| Option              | Default | What it does                                                                                                                                                        |
| ------------------- | ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `idle_timeout`      | 60 s    | How long a connection waits for a request to start. Past it, the connection closes without a word.                                                                  |
| `read_timeout`      | 30 s    | How long a request may take to arrive once started, as a whole: a peer sending a byte now and then gets no more. Past it, it answers 408 and the connection closes. |
| `write_timeout`     | 30 s    | How long a response or a `100 Continue` may take to leave, as a whole. Past it, the connection closes.                                                              |
| `lingering_timeout` | 5 s     | After the last response, what the peer still sends is read and dropped: how long to wait for more of it. Zero closes at once.                                       |
| `lingering_time`    | 30 s    | How long this goes on as a whole, against a peer that keeps sending. Zero closes at once.                                                                           |
| `limits`            | below   | What a request may hold.                                                                                                                                            |
| `on_handler_error`  | none    | Called with what a handler, a middleware or a `body_writer` threw, on the connection's executor. Must not throw.                                                    |
| `drain`             | none    | A stop request here ends the connection gently. `serve` sets it for its own shutdown, replacing what is given here.                                                 |

Lingering is there because closing with the peer's bytes unread would reset the connection, and may
destroy the response before the peer reads it.

Zero disables a timeout, and a negative one has already expired. Timeouts need the timer of the
connection's executor, which `quoll::runtime` provides: see
[thread pools](runtimes.md#the-thread-pool). A body read or written in pieces has `read_timeout` or
`write_timeout` per piece: see [Streaming](streaming.md).

The time a handler takes is not limited: that is the application's to bound, with
`async::with_timeout`, which stops the work and awaits its end.

```cpp
router.get("/report", [](http::request) -> task<http::response> {
  try {
    co_return co_await quoll::async::with_timeout(5s, build_report());
  } catch (std::system_error const &failure) {
    if (failure.code() != std::errc::timed_out) {
      throw;
    }
  }

  http::response too_slow;

  too_slow.status = http::status::gateway_timeout;
  co_return too_slow;
});
```

### `limits`

| Limit                    | Default | What it bounds                                       | Past it |
| ------------------------ | ------- | ---------------------------------------------------- | ------- |
| `max_header_size`        | 64 KiB  | Start line, fields and trailers together             | 431     |
| `max_field_count`        | 100     | Fields and trailers together                         | 431     |
| `max_body_size`          | 8 MiB   | A body read whole                                    | 413     |
| `max_streamed_body_size` | 1 GiB   | A [body read in pieces](streaming.md), not in memory | 413     |

A body too large is refused as soon as its head announces it.

### `serve_options`

| Option                | Default | What it does                                                                                                                |
| --------------------- | ------- | --------------------------------------------------------------------------------------------------------------------------- |
| `connection`          |         | The `connection_options` above.                                                                                             |
| `max_connections`     | 10,000  | How many connections are served at once. Past it, a new one is accepted and closed at once, without a word. Zero: no limit. |
| `shutdown_timeout`    | 30 s    | How long the requests under way may take to finish once asked to stop. Past it, they are stopped. Zero stops them at once.  |
| `on_connection_error` | none    | Called with the exception that ended a connection (a stream exception, `std::bad_alloc`...). Must not throw.                |
| `on_accept_error`     | below   | What an accept failure does.                                                                                                |

`on_accept_error` is a function from `std::error_code` to `task<bool>`, so that it may wait first:
true accepts again, false ends the accept loop by rethrowing the failure. Without one, a failure of
one connection is skipped, running out of something (file descriptors, memory) is retried after
100 ms without a word, and any other failure is rethrown. A rethrow ends the accept loop, and
`serve` throws it once its connections have ended.

## Shutdown

A stop request is graceful. Give `listen` or `serve` a stop token through `co_spawn`, the runtime's to
stop on Ctrl-C, or your own:

```cpp
std::stop_source stopping;

quoll::async::co_spawn(runtime.get_executor(), serve_api(), quoll::async::detached,
                       stopping.get_token());
```

On a stop request:

1. The accept loop ends.
2. Idle connections close at once.
3. Requests under way finish, within `shutdown_timeout`, past which they are stopped. A response not
   started yet says `Connection: close`.
4. The task completes once its connections have ended, so that `listener` and `router` only have to
   outlive it.

## Errors

| What happens                                          | What the peer gets                                                                     | The connection           |
| ----------------------------------------------------- | -------------------------------------------------------------------------------------- | ------------------------ |
| Not valid HTTP                                        | 400                                                                                    | ends                     |
| A header too large                                    | 431                                                                                    | ends                     |
| A body too large                                      | 413                                                                                    | ends                     |
| An unknown method                                     | 501                                                                                    | ends                     |
| `Expect` asking for anything but `100-continue`       | 417                                                                                    | ends                     |
| A request that does not arrive in time                | 408                                                                                    | ends                     |
| A connection idle past `idle_timeout`                 | nothing                                                                                | ends                     |
| A response that does not leave in time                | what has left                                                                          | ends                     |
| A handler or a middleware throws                      | 500, and `on_handler_error` is told                                                    | goes on                  |
| A handler builds a response that cannot be serialized | 500                                                                                    | goes on                  |
| A `body_writer` throws                                | 500 if nothing has left, otherwise [a body cut short](streaming.md#writing-a-response) | ends once it has started |

`Expect: 100-continue` itself is handled for you: a request whose body has not come gets
`100 Continue`, then its body is read. With an [`http::body_reader`](streaming.md#reading-a-request),
this waits for the handler to ask for the body.

## TLS

A certificate and its key make it an HTTPS server. Both files are PEM, the certificate followed by its
chain:

```cpp
co_await quoll::server::listen(
    router, 8443, {.tls = {.certificate_file = "cert.pem", .private_key_file = "key.pem"}});
```

The handshake takes place on the connection's first read, so that a slow peer holds nobody back, and
the idle timeout covers it. A connection ending sends TLS's closing message. Quoll built without
OpenSSL throws `std::logic_error` here (see `QUOLL_TLS` in
[Getting started](getting-started.md#with-cpm)).

## Not there yet

Static files, server-sent events and WebSocket are on [the roadmap](PLAN.md#roadmap), not in Quoll
today. A 2xx answer to CONNECT, and a response announcing more `Content-Length` than its body, are
open points of its [step 2](PLAN.md#step-2-client-and-server-security-and-completeness).
