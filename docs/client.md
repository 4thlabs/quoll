# Client

`http::client` speaks HTTP/1.1 to one server, keeps its connections and reuses them. It models
`http::HttpTransport`, which `rest::client` calls through: for typed requests and responses, see
[REST](rest.md). Link `quoll::runtime` and `quoll::client`.

The examples here assume:

```cpp
namespace async = quoll::async;
namespace http = quoll::http;
using namespace std::chrono_literals;
```

## Sending a request

```cpp
http::client client{"https://api.example.com/v1"};

auto const users = co_await client.get("/users?limit=20");
auto const response = co_await client.post("/users?notify=true", R"({"name":"Ada"})", "application/json");

if (response.status == http::status::created) {
  std::cout << response.body << '\n';
}
```

`get`, `head` and `delete_` take a target, and `post`, `put` and `patch` a target, a body and, if it has
one, its type. For anything else, such as fields of its own, or a request body in pieces, `send` takes a
whole request, and `stream` reads a [response in pieces](streaming.md):

```cpp
http::request request;

request.method = http::verb::post;
request.target = "/users";
request.fields.set("Authorization", token);
request.fields.set("Content-Type", "application/json");
request.body = R"({"name":"Ada"})";

auto const created = co_await client.send(request);
```

Every response is a result, error statuses included. A target is in origin form, starting
with `/`. A `Host` field naming the server is added unless the request has one, and `Content-Length`
when it has a body or is a POST, PUT or PATCH, unless its fields declare a framing.

A client opens its connections through the network of the executor that sends, so that it needs
nothing from you. Copies share their connections. A task copies what it needs, so that the client
does not have to outlive it. What holds connections belongs in a coroutine, not in `main()`: see
[How a program runs](getting-started.md#how-a-program-runs). All of these may be called
concurrently, from any thread, if the connector allows it.

## URLs

The URL is `http[s]://host[:port][/path]`, an IPv6 address in brackets:

```cpp
http::client local{"http://127.0.0.1:8080"};
http::client v6{"http://[::1]:8080"};
http::client api{"https://api.example.com/v1"};
```

A path goes before the target of every request, as it is. With the last one, `/users` is `/v1/users`,
and `/` is `/v1/`, never `/v1`. It is no boundary: a target with dot segments of its own can climb out
of it on a server or a proxy resolving them.

Anything else throws `std::invalid_argument` here: another URL, or a path that would not read as
itself (an invalid escape, a dot segment, a NUL, an empty segment, what a proxy could read as another server...). So do a host
with an empty label, as `a..b`, and a URL with port 0.

To connect some other way, give a connector, anything modelling `io::Connector`, before the URL, or
with a host and a port, which count as `http` for a redirect. An `https` URL needs an
`io::TlsConnector`, and throws `std::invalid_argument` otherwise:

```cpp
http::client over_url{my_connector{}, "http://api.example.com/v1"};
http::client over_host{my_connector{}, "example.com", 80};
```

## Options

The options come after the URL:

```cpp
http::client client{"https://api.example.com",
                    {.max_connections = 16, .max_redirects = 3, .response_timeout = 5s}};
```

| Option                 | Default   | What it does                                                                                          |
| ---------------------- | --------- | ----------------------------------------------------------------------------------------------------- |
| `max_idle_connections` | 8         | Connections kept open once their response is read. Zero closes each connection after its response.    |
| `max_connections`      | 0         | How many connections are open at once, in use or idle. Zero: no limit.                                |
| `max_idle_for_unsafe_methods` | 1 s | How long a connection may have been idle for a POST, a PATCH or another method not idempotent to take it. Zero: such a request always opens its own. |
| `max_redirects`        | 0         | How many redirects a request follows. Zero: a 3xx response is a result like the others.               |
| `connect_timeout`      | 30 s      | How long opening a connection may take, name resolution included, and waiting for `max_connections`.  |
| `response_timeout`     | 60 s      | How long a request may take once on a connection, from sending it to reading the end of its response. |
| `limits`               | see below | The sizes of a response, against servers sending unbounded ones.                                      |
| `tls`                  | see below | How the servers of `https://` URLs are checked.                                                       |

`limits` are those of the [server](server.md#limits), bounding responses: 64 KiB of header, 100 fields,
8 MiB of body. A body read through [`stream()`](streaming.md#reading-a-response) has no limit by
default, as memory does not hold it and the caller is reading it.

Zero disables a timeout, and a negative one has already expired. Timeouts need the timer of the
executor that sends, which `quoll::runtime` provides: on one without a timer, as `async::run_loop`, they do not apply.

`response_timeout` applies per attempt, so that a connection found closed and tried again counts
anew, and each request of a redirect chain has its own. For a deadline over the whole of a `send`,
use `async::with_timeout`:

```cpp
auto const response = co_await async::with_timeout(2s, client.send(request));
```

## Keep-alive and retries

Connections are kept open once their response is read, up to `max_idle_connections`, and the next
requests reuse them. A request waits for no other: it opens a new connection when none is idle, unless
`max_connections` says otherwise. A connection is not reused after a request or a response that closes
it (`Connection: close`, HTTP/1.0), a response delimited by the end of the stream, a switch of protocol
or a stream error.

A server may close an idle connection at any time, which shows only once a request is sent on it. Every
request whose body is whole takes an idle connection, as Go's client and Boost.Burl do; one whose body comes
in pieces (`body_writer`) has one of its own, as its writer runs once. A request that is not idempotent
takes only a connection idle for less than `max_idle_for_unsafe_methods`, one second by default, well
below the servers' idle timeouts (5 s for Node, 60 s for `http::server`): Go's client finds a closed
connection with a reader always waiting on it, which Quoll's client does not have.

| Method                                 | When its idle connection turns out closed before any response, or answers 408                            |
| -------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| GET, HEAD, PUT, DELETE, OPTIONS, TRACE | The request is sent again, once, on a new connection, and the other idle connections, older, are closed |
| POST, PATCH and the others             | Sent again on a new connection only if none of it was written. Otherwise the request fails with the stream's error, or the 408 is its response, and its connection is not kept: the server may have received it, so it is not sent twice, and the caller decides. The older idle connections are closed |

A request that times out is not sent again, as the server may be working on it, and its connection is
closed. Nothing else is retried: a connection that fails otherwise is a failure of the request.

## `max_connections`

Past `max_connections`, a request waits for a request on a connection to end, and an idle connection
is closed to make room for a new one.

- A stop request ends the wait, and so does `connect_timeout`, which then starts again to connect.
- Without a timeout or a timer, a server that does not answer holds up those that wait.
- A [streamed response](streaming.md#reading-a-response) holds its place until its body has come to its
  end or it goes. Read or drop one before a request that needs its place.
- The executors that send have to take a `post()` from another thread, as `quoll::runtime`'s does, and
  `run_loop`'s does not.

## Redirects

Redirects are not followed by default. With `max_redirects`, a 301, 302, 303, 307 or 308 response
whose `Location` names the client's scheme and the host and port of the request's `Host` field
sends the request again there. Past `max_redirects`, the request fails with
`http::error::too_many_redirects`.

- 307 and 308 keep the method. 303 makes it a GET, but for a HEAD, and 301 and 302 make a POST a GET,
  as browsers do.
- When the method changes, the body goes, with its `Content-*` and framing fields, and so do the
  preconditions (`If-*`, `Range`). The other fields stay, `Authorization` included, as they go to the
  same server.
- A `Location` naming another server or scheme, empty, or that cannot be read, leaves the 3xx response
  as the result. So does a request with a `body_writer`, and anything sent through `stream()`.
- Only the last response comes back, and the others are dropped with their `Set-Cookie`: a client
  keeping cookies leaves `max_redirects` at 0.

## TLS

`https://` checks the server against the system's authorities, and that its certificate names the
host. To trust a private authority, or for a development setup only, to accept anyone:

```cpp
http::client internal{"https://service.internal", {.tls = {.ca_file = "authority.pem"}}};
http::client development{"https://localhost:8443", {.tls = {.verify_peer = false}}};
```

A server closing without TLS's closing message changes nothing for a response whose length is known,
nor for a kept connection. It fails a response delimited by the end of the stream, with
`std::errc::connection_reset`, as anyone on the way could have cut it, and it would look complete.

Quoll built without OpenSSL throws `std::logic_error` from the task (see `QUOLL_TLS` in
[Getting started](getting-started.md#with-cpm)). A connector that cannot open TLS connections makes
the constructor throw `std::invalid_argument`.

## Failures

Whatever keeps a response from coming throws from the task:

| What happens                                  | What is thrown                                                                                                                         |
| --------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| Connecting fails                              | As the connector reports it: `std::system_error` with asio's error code, through the runtime                                           |
| The server does not pass the TLS checks       | The same                                                                                                                               |
| The connection fails while sending or reading | `std::system_error`: the stream's error (`std::errc::connection_reset`...) or `io::error::end_of_stream`                               |
| A response that cannot be read                | `std::system_error`: the parser's error (`http::error::partial_message`, `http::error::body_too_large`, `http::error::bad_message`...) |
| Too many redirects                            | `std::system_error`: `http::error::too_many_redirects`                                                                                 |
| A timeout                                     | `std::system_error`: `std::errc::timed_out`, and the connection closes                                                                 |
| A stop request on the task                    | `std::system_error`: `std::errc::operation_canceled`, and the connection closes                                                        |
| A request that cannot be sent                 | `std::invalid_argument`, before the task runs: a target not in origin form, what `http::serialize` rejects                             |

```cpp
try {
  auto const response = co_await client.send(request);

  std::cout << response.body << '\n';
} catch (std::system_error const &failure) {
  if (failure.code() == std::errc::timed_out) {
    std::cerr << "too slow\n";
  } else {
    std::cerr << failure.what() << '\n';
  }
}
```

Interim 1xx responses are skipped, up to 8 of them, but for a 101 switch of protocol, which comes back
as the response.

## In a test, and in a browser

`http::local_transport` hands requests to a router of the same program, without serializing them, for
in-process calls and tests of a client against its server. What a handler throws comes out of the task,
where a connection would answer 500, so that a test sees the failure itself.

```cpp
http::router router;

router.get("/hello", [](http::request) {
  http::response response;

  response.body = "hello";
  return response;
});

http::local_transport const local{router};
http::request request;

request.target = "/hello";

auto const response = co_await local.send(request);
```

It keeps its own copy of the router, so that nothing has to outlive it: add the routes first, as one
added afterwards is not served.

In a browser, a client from a URL goes through `fetch`, and the same code does the same thing, but for
[what `fetch` keeps to itself](runtimes.md#httpclient-in-a-browser).
