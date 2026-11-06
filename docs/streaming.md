# Streaming

A body does not have to fit in memory. A server can read a request and write a response in pieces, and
a client can write a request and read a response in pieces, each holding one piece at a time. A peer
sends no faster than the other side reads, as the next piece is only read when asked for, and a write
completes once its piece has left.

| To              | On the server                       | On the client                                            |
| --------------- | ----------------------------------- | -------------------------------------------------------- |
| Read in pieces  | a handler takes `http::body_reader` | `client.stream(request)`, then `response.body.read()`    |
| Write in pieces | `response.body_writer`              | `request.body_writer`                                    |

The examples here assume:

```cpp
namespace async = quoll::async;
namespace http = quoll::http;
using namespace std::chrono_literals;
```

Streaming belongs to the router and to the transports. A [REST contract](rest.md) reads and writes its
bodies whole.

## Reading a request

A handler taking an `http::body_reader` as its last parameter reads the body as it comes. The request
is still there if it takes it first, with `request.body` empty, for the middlewares too:

```cpp
router.post("/users/{id}/avatar", [](std::int64_t id, http::body_reader body) -> async::task<http::response> {
  while (auto const piece = co_await body.read()) {
    co_await store_piece(id, *piece);
  }

  co_return http::response{};
});
```

`read()` gives a view, valid up to the next `read()`, and nothing at the end of the body, as often as
it is asked. One `read()` at a time, and a body is read once. The trailers of a body in pieces are not
handed over.

| When                                      | What happens                                                                                                                                                                                                                                                                                     |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| The handler leaves some of it unread      | The connection closes after its response. A body that had all come with its head leaves it open.                                                                                                                                                                                                 |
| `Expect: 100-continue`                    | `100 Continue` leaves at the handler's first `read()`, so that a handler refusing the request saves the upload. A failure while it leaves is a failure of that `read()`.                                                                                                                         |
| The peer goes quiet or too slow           | `read_timeout` and `min_read_rate` apply as in [Server](server.md#connection_options), across the pieces: a transfer takes as long as it keeps coming at the minimum rate. Past it, `read()` throws, and the request answers 408.                                                                |
| The body is too large                     | Past `limits.max_streamed_body_size`, 1 GiB by default: a length that announces it is refused before the handler runs, otherwise `read()` throws. Either way, the request answers 413.                                                                                                           |
| The body is cut short or the stream fails | `read()` throws `std::system_error`, and goes on throwing. The request is then answered as one refused as it is read would be, whatever the handler made of it, as it could not know what the connection can still carry.                                                                        |
| The handler stops reading on its own      | A cancellation of the read, as its own `async::with_timeout` is, ends the reading: a stream does not take it up again, and `read()` then throws. The handler's response stands, and the connection closes after it. Over TLS, whose session a cancelled read leaves unusable, it may not arrive. |
| A stop request on the connection          | `read()` throws `operation_canceled`. That is no failure of the handler: `on_handler_error` is not told, and no 500 goes out.                                                                                                                                                                    |

A handler with a patience of its own:

```cpp
router.post("/upload", [](http::body_reader body) -> async::task<http::response> {
  http::response response;

  try {
    while (auto const piece = co_await async::with_timeout(1s, body.read())) {
      co_await store_piece(0, *piece);
    }
  } catch (std::system_error const &failure) {
    if (failure.code() != std::errc::timed_out) {
      throw;
    }

    response.status = http::status::gateway_timeout;
  }

  co_return response;
});
```

A `body_reader` is a small handle, copied freely, and it stays valid until the response has left: a
`body_writer` may read it, [as an echo does](#writing-a-response). What reads through it does so on the
executor of the handler's task. Moved to another, without a timer, it has no read timeout.

## Writing a response

`response.body_writer` writes the body through an `http::body_stream`, once the handler has ended:

```cpp
router.get("/export", [](http::request) {
  http::response response;

  response.fields.set("Content-Type", "text/csv");
  response.body_writer = [](http::body_stream out) -> async::task<> {
    auto const rows = load_rows();

    for (auto const &row : rows) {
      co_await out.write(row);
    }
  };
  return response;
});
```

`write()` completes once the piece has left, and the piece stays valid until then. One `write()` at a
time. The head leaves with the first piece. An empty piece sends it at once, for a stream to announce
itself before its first data.

- **Framing.** Without a `Content-Length` the body goes in chunks, or to HTTP/1.0 up to the end of the
  connection, which its peer cannot tell from a cut. With one, exactly that many bytes are expected:
  more throws, and fewer cut the connection, so that its peer sees a body cut short.
- **HEAD.** The writer is not called.
- **Lifetimes.** It runs after its handler has ended. What it captures by value lives until its task
  ends. A `std::string_view` of a path parameter, or a reference to the request, does not. An
  `http::body_reader` does:

```cpp
router.post("/echo", [](http::body_reader body) {
  http::response response;

  response.body_writer = [body](http::body_stream out) -> async::task<> {
    while (auto const piece = co_await body.read()) {
      co_await out.write(*piece);
    }
  };
  return response;
});
```

With `Expect: 100-continue`, such a writer writes and reads in turn, as `100 Continue` and a piece must
not leave at once.

| When                                          | What happens                                                                                                                                                                                     |
| --------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| The writer throws before anything has left    | The client gets a 500, and `on_handler_error` is told.                                                                                                                                           |
| The writer throws once the body has started   | The connection is cut, as a peer must not take what came for all of it, and `on_handler_error` is told.                                                                                          |
| The writer ends short of its `Content-Length` | As the two rows above: a 500 if it wrote nothing, and otherwise the connection is cut. `on_handler_error` is told either way.                                                                    |
| The peer is gone, or a piece does not leave   | `write()` throws `std::system_error`, and goes on throwing. Let through the writer, it ends the connection, and is no failure of the writer's.                                                   |
| A piece takes too long to leave               | `write_timeout` and `min_write_rate` apply as in [Server](server.md#connection_options), across the pieces. Past it, the connection closes. The time the writer takes between pieces is its own. |
| A stop request on the connection              | What the writer awaits throws `operation_canceled`, a `write()` or a wait of its own. That is no failure of the writer: the connection ends, and `on_handler_error` is not told.                 |

## Reading a response

`client.stream(request)` gives the response once its head has come. Its body is read in pieces:

```cpp
http::client client{"https://downloads.example.com"};
http::request request;

request.target = "/big.iso";

auto response = co_await client.stream(request);

if (response.status != http::status::ok) {
  co_return;
}

while (auto const piece = co_await response.body.read()) {
  co_await save(*piece);
}
```

This produces an `http::streamed_response`: `status`, `reason`, `version`, `fields`, `body`, and
`trailers`, the fields that came after a chunked body, once `body` has come to its end. What follows is
for `http::client`. [In a browser](#in-a-browser), the response is the same, with other rules.

- **The connection.** Read to its end, or with nothing to read, its connection goes back to the
  client. Destroyed before, the connection closes, which is how to give up on a body. Until then it
  holds its place in [`max_connections`](client.md#max_connections).
- **A piece.** It is valid up to the next `read()`, and for as long as `body`. One `read()` at a time,
  among all the copies of `body`, which share what reads it, so that it may be taken out of its response.
  Its last copy goes on the loop of what reads it, as it holds the connection.
- **Limits.** `limits.max_streamed_body_size` has no limit by default on a client, as memory does not
  hold the body.
- **Redirects.** A 3xx response is a result, whatever `max_redirects` says, as its body is for the
  caller to read or drop.

Connections and retries are as in `send()` up to the head. From there:

| When                                  | What happens                                                                                                                                                  |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| The connection closes during the body | `body.read()` throws `std::system_error`, as what came is the caller's, and it is not tried again. The connection closes, and leaves its place to others.     |
| A piece takes too long to come        | `response_timeout` applies up to the head, then per piece: a transfer takes as long as it keeps coming. Past it, `body.read()` throws `std::errc::timed_out`. |
| A stop request or a cancellation      | `body.read()` throws `std::errc::operation_canceled`, and the connection closes. Reading ends there, as a stream does not take it up again.                   |

## Writing a request

`request.body_writer` writes the body of a request in pieces, in chunks unless a `Content-Length` says
how much comes:

```cpp
http::request request;

request.method = http::verb::put;
request.target = "/backups/today";
request.body_writer = [](http::body_stream out) -> async::task<> {
  auto const parts = load_rows();

  for (auto const &part : parts) {
    co_await out.write(part);
  }
};

auto const response = co_await client.send(request);
```

It goes on a connection of its own, as the writer runs once, so that nothing is sent again, and a 3xx
response is a result, whatever `max_redirects` says. What it captures has to live until the task of the
request ends.

The response is read meanwhile, so that a server refusing a body has its answer heard. `send()` keeps
writing until that response is whole, then stops the writer through its stop token.

| When                                             | What happens                                                                                                                                       |
| ------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| The response is whole before the end of the body | The writer is stopped, and `write()` then throws. The task ends with the writer's, and the answer stands, whatever the writer throws from then on. |
| The server closes                                | `write()` throws. The answer is still the result, if it gave one.                                                                                  |
| The writer throws anything else                  | The connection is cut, and it comes out of the task.                                                                                               |
| The writer ends short of its `Content-Length`    | The same.                                                                                                                                          |
| A piece takes too long                           | `response_timeout` applies to each piece, then to the response from the end of the body. The time the writer takes between pieces is its own.      |
| A stop request or a timeout on the task          | `std::errc::operation_canceled` or `std::errc::timed_out`, and the connection closes.                                                              |

`send()` throws `std::invalid_argument`, before its task runs, for a request with both a `body` and a
`body_writer`, or over HTTP/1.0 with no `Content-Length`.

### `stream()` with a `body_writer`

> [!IMPORTANT]
> `stream()` stops a `body_writer` at the head of the response, as the caller goes on to read by itself.
> For all of the body to leave, the server has to read the whole of the request before it answers.
>
> A server that answers while it reads, as an echo does, should be called through `send()`. Through
> `stream()`, the rest of the request does not leave: the server waits for it while the client waits for
> the response, until a timeout ends it, the server's `read_timeout` or the client's `response_timeout`,
> and `body.read()` then throws.

Over TLS, a write that this cuts short leaves its session unusable: the head is there, and
`body.read()` may then fail at once.

## In a browser

The code above runs as it is, as `http::client` goes through `fetch` there:

```cpp
http::client client{"https://api.example.com"};
http::request request;

request.target = "/feed";

auto response = co_await client.stream(request);

while (auto const piece = co_await response.body.read()) {
  show(*piece);
}
```

Its task completes when `fetch()` resolves, and a piece is what one JavaScript read of `response.body`
gives, so that nothing holds the body whole. The rest differs from what a connection does:

| When                            | What happens                                                                                                                                                                                                                                             |
| ------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A read fails                    | `body.read()` throws `std::system_error` with `std::errc::io_error` and `fetch`'s message, what came before it first. The next reads throw the same, and the fetch is aborted and released.                                                              |
| A stop request during a read    | The fetch is aborted, and `body.read()` throws `std::errc::operation_canceled`. The next reads throw the same.                                                                                                                                           |
| The server goes quiet           | `response_timeout` applies to each piece, as it does natively. `async::with_timeout` around a read ends the reading as a whole, not that piece: it throws `std::errc::timed_out`, the fetch is aborted, and the next reads throw `std::errc::operation_canceled`. |
| Giving up on a body             | The last copy of `body` to go before the end aborts the fetch and releases it.                                                                                                                                                                           |
| The end of the body, or no body | The fetch is released at once.                                                                                                                                                                                                                           |
| Limits and trailers             | `max_streamed_body_size` applies, as it does natively. `trailers` is not null and stays empty, as `fetch` does not expose them.                                                                                                                 |

A request does not go in pieces. A `request.body_writer` runs to its end before the fetch, through `send()`
as through `stream()`, and what it wrote goes out whole, as a fetch sending in pieces needs HTTP/2, and is
not in every browser. So memory holds that body, `write()` does not wait for a peer, and `stream()` has
nothing left to stop at the head. A stop request or an `async::with_timeout` around the task ends the
writer at its next `write()`, which throws.

See [`http::client` in a browser](runtimes.md#httpclient-in-a-browser) for what else `fetch` changes.

## In a test

`http::local_transport` has no `stream()`. It runs a `request.body_writer` to its end before the handler,
which finds what it wrote in `request.body`, or as one piece through its `body_reader`. What a writer on a
response writes is gathered into `response.body`.
