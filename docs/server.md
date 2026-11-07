# Server

`http::server` serves routes over connections, in HTTP/1.x, over TCP or TLS. Link `quoll::runtime` and
`quoll::server`.

```cpp
http::server server;

server.get("/hello/{name}", hello);
co_await server.listen(8080);
```

Its routes live in a router, which maps requests to handlers and does no I/O. The examples of routes
below name it `router`, and read the same on an `http::router` and on an `http::server`, which has the
same functions. To serve a typed API rather than routes by hand, see [REST](rest.md).

The examples here assume:

```cpp
namespace async = quoll::async;
namespace http = quoll::http;
namespace io = quoll::io;
namespace rest = quoll::rest;
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

router.post("/users/{id}/cards", [](http::request request, std::int64_t id) -> async::task<http::response> {
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
`http::response`, or `async::task<http::response>` when it has something to await.

All of this is checked at compile time: the path is a string literal and a valid template, with as
many parameters as the handler takes, which has types (no `auto`). A path made at run time goes
through `add()`, which also hands over the name of the route and its query:

```cpp
router.add(http::verb::get, "/search", "search",
           [](http::request, http::route_match match) -> async::task<http::response> {
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
thread.

## Responses

`http::response` is an aggregate: `status`, `reason`, `version`, `fields`, `body`, `trailers`, and
`body_writer` to write the body in pieces (see [Streaming](streaming.md)). `Content-Length` is added, `0`
for an empty body, unless its fields declare a framing or its status has no body (1xx, 204, 304). Quoll
adds no `Date` or `Server` field.

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
router.use([](http::request request, http::next const &next) -> async::task<http::response> {
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
- A handler taking an [`http::response_writer`](streaming.md#writing-a-response) runs once the middlewares have returned: they do
  not see its status, catch what it throws or bound its time.
- What it changes in the request reaches the handler, but not what comes of its method and target:
  the route, its parameters and its query are those that came.

## Static files

`http::static_files` is a middleware serving a directory, for the GET and HEAD requests under a prefix.

```cpp
http::router router;

router.get("/api/hello", hello);
router.use(http::static_files("/assets", "./public"));
router.use(http::static_files("/", "./site"));
```

It only answers what the router itself answers 404 to, once the middlewares added after it have run:

- A route always wins, and a request a route answers costs no look at the disk.
- Where it stands among the middlewares protects nothing by accident: one added after it that refuses a request refuses its
  file too, and what they add to the 404, CORS headers for instance, stays on the file's response.
- A path the router refuses (`..`, `%00`) has had its 400.
- A later middleware answering a 404 of its own rather than awaiting `next`, to hide something, has answered: only the
  router's own counts, which one that awaits `next` and then changes has not replaced.
- Of several on one path, the last added looks first, and a file that is not there leaves the 404 to the one before. That
  is the other way round from Express.

| Option          | Default      | What it does                                                                                                              |
| --------------- | ------------ | ------------------------------------------------------------------------------------------------------------------------- |
| `index`         | `index.html` | What a request for a directory serves. Empty: such a request stays a 404.                                                 |
| `dotfiles`      | `false`      | Whether names starting with a dot are served. `.env` and `.git` are not, by default, nor what a link leads to among them. |
| `content_types` | empty        | The `Content-Type` of an extension, lower case with no dot, in addition to those known, and over them.                    |

What comes out:

- **In pieces.** A file goes out by 64 KiB, so that what it costs in memory does not depend on its size, and the
  [minimum rates](#connection_options) apply as to any response. It is read on threads of the runtime's own, so that a slow
  disk does not hold up the loop: see [Runtimes](runtimes.md).
- **What describes it.** `Content-Type` from its extension, `application/octet-stream` otherwise, `Content-Length`,
  `Last-Modified`, `ETag` and `Accept-Ranges`.
- **A cache.** `If-None-Match` and `If-Modified-Since` answer 304, the first taking over the second. `If-Match` and
  `If-Unmodified-Since` answer 412 when the file is no longer the one asked for, for resuming a download safely.
- **A part.** A `Range` of one interval answers 206, and 416 out of the file, for resuming a download or seeking in a video.
  Several intervals, or what cannot be read, send it whole. `If-Range` holds it to the file it was for.
- **A directory.** Without its final slash, a redirect to it, if it has an index.

The root is looked at again for each request, so that a link flipped to deploy a new release is followed. Nothing outside it
is served, a symbolic link leading out of it included, and neither is what is not a regular file:
all of that is the same 404 as what does not exist. There is no listing of a directory, and no compression.

## Serving

`http::server` holds its routes, added with the same functions as a router's, or starts from a copy of a
router, for one built apart or shared by several servers. Both take the same options:

```cpp
http::server simple;
http::server simple_with_options{{.max_connections = 1000}};
http::server custom{router};
http::server custom_with_options{router, {.max_connections = 1000}};

simple.get("/users/{id}", show_user);
rest::mount<api>(simple, users_service{});
```

Its `listen` listens on a port and serves until a stop request. It listens through the network of
the executor it runs on, so that it needs nothing else. It takes the loopback interface by default:
ask for `0.0.0.0` or `::` to be reachable from outside.

```cpp
http::server server{router};

co_await server.listen(8080, {.address = "0.0.0.0"});
```

It throws `std::system_error` when the address cannot be bound or the certificate loaded.

Nothing has to outlive what a server starts: its tasks share what it holds, and it keeps its own copy of a
router it is given. Add the routes first. One added to the server once it serves throws
`std::logic_error`, and one added to the router it copied is not served. A handler with a state of its
own is copied with its router: for two servers of one router to share it, hold it in a `std::shared_ptr`.
A handler does not capture a copy of its server, which would keep it alive forever, with all it holds.

A copy of a server is the same server. It may listen on several ports, each `listen` or `serve` being a task
of its own, and `max_connections` counts for all of them, on a server's connections as a whole. `server.connections()` says how many it
serves now. A stream given to `serve_connection` counts, and is never refused. `on_accept_error` and `on_connection_error`
may be called concurrently, when the server's tasks run on several threads.

To know the port before serving, or that the server is ready, listen first and then `serve`. With
port 0, the system picks a free one:

```cpp
auto listener = co_await io::listen(0);

std::cout << "listening on " << listener.local_port().value_or(0) << std::endl;

http::server server{router};

co_await server.serve(std::move(listener));
```

`serve` takes over anything modelling `io::Listener`: what you need of it is read before. Each connection runs as a task of its own, so that
connections are served concurrently, one request at a time on each.

`server.serve_connection` serves one connection, over anything modelling `io::ByteStream`, until it ends.
This is how a test runs a server over a memory stream, with a peer beside it:

```cpp
auto const peer = [](io::memory_stream stream) -> async::task<http::response> {
  std::string_view const sent = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
  std::string buffer;
  http::response_parser parser;

  (void)co_await io::write(stream, std::as_bytes(std::span{sent}));
  (void)co_await http::read(stream, buffer, parser);
  co_return parser.release();
};

auto [client_side, server_side] = io::make_memory_stream_pair();

auto [served, response] = co_await async::when_all(
    http::server{router}.serve_connection(std::move(server_side)), peer(std::move(client_side)));
```

The peer's stream closes with its task, which ends the connection.

> [!NOTE]
> `quoll::runtime` runs everything on the thread that calls `run()`. A handler that blocks that
> thread holds up every connection: what takes time is awaited, not waited for.

## Options

The server takes `serve_options`, holding `connection_options`, and `listen` takes `listen_options`, which say where:

```cpp
http::server server{router, {.connection = {.idle_timeout = 30s,
                                            .read_timeout = 10s,
                                            .on_handler_error = [](std::exception_ptr failure) { report(failure); }},
                             .max_connections = 1000}};

co_await server.listen(8080, {.address = "0.0.0.0"});
```

### `connection_options`

| Option              | Default  | What it does                                                                                                                                                                                                                              |
| ------------------- | -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `idle_timeout`      | 60 s     | How long a connection waits for a request to start. Past it, the connection closes without a word.                                                                                                                                        |
| `read_timeout`      | 30 s     | How long a request may wait for its peer once started, and not the time a handler takes between two reads. Past it, it answers 408 and the connection closes. It starts again once `100 Continue` has left, as the client waits for that. |
| `min_read_rate`     | 16 KiB/s | What lets a request take as long as it keeps coming: each byte of its body adds to what `read_timeout` has left, up to `read_timeout` itself. See below.                                                                                  |
| `write_timeout`     | 30 s     | How long a response or a `100 Continue` may wait for its peer to take it, and not the time a `body_writer` takes between two writes. Past it, the connection closes.                                                                      |
| `min_write_rate`    | 16 KiB/s | The same for what leaves, every byte earning. A write that leaves within 100 ms gives back all of `write_timeout`.                                                                                                                        |
| `lingering_timeout` | 5 s      | After the last response, what the peer still sends is read and dropped: how long to wait for more of it. Zero closes at once.                                                                                                             |
| `lingering_time`    | 30 s     | How long this goes on as a whole, against a peer that keeps sending. Zero closes at once.                                                                                                                                                 |
| `limits`            | below    | What a request may hold.                                                                                                                                                                                                                  |
| `on_handler_error`  | none     | Called with what a handler, a middleware or a `body_writer` threw, on the connection's executor. Must not throw.                                                                                                                          |
| `drain`             | none     | A stop request here ends the connection gently. `serve` sets it for its own shutdown, replacing what is given here.                                                                                                                       |

Lingering is there because closing with the peer's bytes unread would reset the connection, and may
destroy the response before the peer reads it.

Zero disables a timeout, and a negative one has already expired. Timeouts need the timer of the
connection's executor, which `quoll::runtime` provides: see
[thread pools](runtimes.md#the-thread-pool).

The minimum rates make one rule of slow links and slow peers. At the rate or above, a transfer has all
the time it needs, 8 MiB in a minute included. A silence still ends it within the timeout, whatever came
before it. Below the rate, a peer runs out of time, in a body read whole as in pieces, and a head or the
framing of a body earns nothing.

At 0, a request or a response has its timeout as a whole, and a body in pieces has it per piece. That is
what a peer uploading little and slowly on purpose needs, 1 KiB every 5 s for instance, which the default
ends after about 30 s. It holds for all the routes of a connection, not for one. Writes do not need it: on a
stream of small writes to a peer that takes them, each gives back all of the timeout, whatever the time
between them.

The time a handler takes is not limited: that is the application's to bound, with
`async::with_timeout`, which stops the work and awaits its end.

```cpp
router.get("/report", [](http::request) -> async::task<http::response> {
  try {
    co_return co_await async::with_timeout(5s, build_report());
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
| `max_connections`     | 10,000  | How many connections the server serves at once, through all of its `listen` and `serve`. Past it, a new one is accepted and closed at once, without a word. Zero: no limit. |
| `shutdown_timeout`    | 30 s    | How long the requests under way may take to finish once asked to stop. Past it, they are stopped. Zero stops them at once.  |
| `on_connection_error` | none    | Called with the exception that ended a connection (a stream exception, `std::bad_alloc`...). Must not throw.                |
| `on_accept_error`     | below   | What an accept failure does.                                                                                                |

`on_accept_error` is a function from `std::error_code` to `async::task<bool>`, so that it may wait first:
true accepts again, false ends the accept loop by rethrowing the failure. Without one, a failure of
one connection is skipped, running out of something (file descriptors, memory) is retried after
100 ms without a word, and any other failure is rethrown. A rethrow ends the accept loop, and
`serve` throws it once its connections have ended.

## Shutdown

A stop request is graceful. Give `listen` or `serve` a stop token through `co_spawn`, the runtime's to
stop on Ctrl-C, or your own:

```cpp
std::stop_source stopping;

async::co_spawn(runtime.get_executor(), serve_api(), async::detached, stopping.get_token());
```

On a stop request:

1. The accept loop ends.
2. Idle connections close at once.
3. Requests under way finish, within `shutdown_timeout`, past which they are stopped. A response not
   started yet says `Connection: close`.
4. The task completes once its connections have ended, so that nothing has to outlive it.

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

What a handler answers is replaced, and `on_handler_error` told, when its peer would read it as something else: a 2xx to CONNECT,
a tunnel, which is not offered, answers 501; a 1xx, which is no final response, a `Content-Length` with no body, but to HEAD or
as a 304, a `Content-Length` that is invalid or other than the body, a body beside what writes one, and to HTTP/1.0 a
`Transfer-Encoding` other than chunked, answer 500.

An HTTP/1.1 request with no `Host` or with several, an HTTP/1.0 one with several, and one whose `Host` is no host and port, answers
400, as RFC 9112 asks, as does a target with a fragment. HTTP/1.0 with a `Transfer-Encoding` ends the connection after its response.

`Expect: 100-continue` itself is handled for you: a request whose body has not come gets
`100 Continue`, then its body is read. With an [`http::body_reader`](streaming.md#reading-a-request),
this waits for the handler to ask for the body, unless its response has a writer: see [Streaming](streaming.md#reading-a-request).

## TLS

A certificate and its key make it an HTTPS server. Both files are PEM, the certificate followed by its
chain:

```cpp
http::server server{router};

co_await server.listen(8443, {.tls = {.certificate_file = "cert.pem", .private_key_file = "key.pem"}});
```

The handshake takes place on the connection's first read, so that a slow peer holds nobody back, and
the idle timeout covers it. A connection ending sends TLS's closing message. Quoll built without
OpenSSL throws `std::logic_error` here (see `QUOLL_TLS` in
[Getting started](getting-started.md#with-cpm)).

## Not there yet

Server-sent events and WebSocket are on [the roadmap](PLAN.md#roadmap), not in Quoll today. Nor are tunnels: a 2xx answer to
CONNECT, which would be one, answers 501.
