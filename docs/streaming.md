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
| `Expect: 100-continue`                    | `100 Continue` leaves at the handler's first `read()`, so that a handler refusing the request saves the upload. A writer may read as it writes, so that it leaves before one starts, unless the status set is 300 or more, or a `body_writer` does not run to HEAD: nothing invites a body then, and reading it fails at once. |
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

A handler takes an `http::response_writer` as its last parameter, and returns a `task<>`:

```cpp
router.get("/export/{name}", [](std::string name, http::response_writer out) -> async::task<> {
  out.fields.set("Content-Type", "text/csv");

  for (auto const &row : load_rows(name)) {
    co_await out.write(row);
  }
});
```

It runs once the router has answered, awaited by what serves the connection, so that its parameters live until it ends,
references and views too. The head leaves with its first `write`: past that, changing its status or fields does nothing. A
middleware has therefore returned by then: it does not see the status, catch what the handler throws or bound its time, and it adds
to the fields, which the handler's replace by name, but for `Set-Cookie` and `Vary`, which add up.
What it throws before its first `write` answers 500, and after it cuts the connection. If it ends without writing, its response has
an empty body. To HEAD, it runs up to its first `write`, which sends the head and throws `std::errc::operation_canceled`. A route may
read as it writes, with `(http::body_reader in, http::response_writer out)`. With `Expect: 100-continue`, `100 Continue` leaves
before it starts, so that such a route invites its body even if it goes on to answer without it. A middleware finds such a handler in `response.late_head_writer`, which a
`response.body_writer` it sets replaces, and it runs once.

To build a response ahead of time, or to write from a middleware, `response.body_writer` writes the body through an
`http::body_stream`, once the handler has ended:

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

With `Expect: 100-continue`, `100 Continue` leaves before such a writer starts.

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

## Server-sent events

A `text/event-stream` body is a stream of events, which a server writes and a client reads in pieces, as
any other. `http::event_writer` writes them through a handler's `response_writer`:

```cpp
router.get("/feed", [](http::response_writer out) -> async::task<> {
  http::event_writer events{std::move(out)};

  co_await events.open();

  auto const pending = co_await changes();

  for (auto const &change : pending) {
    co_await events.send({.type = "change", .data = to_json(change), .id = change.id});
  }
});
```

It sets `Content-Type: text/event-stream`, and `X-Accel-Buffering: no`, which tells a proxy such as nginx
to pass each event on as it comes, both unless the handler set them: a middleware's are replaced, as a
handler's fields replace them, as a stream needs them. A status and other fields, a `Cache-Control:
no-store` say, go on the `response_writer` before it is given, with nothing written through it yet: it
throws `std::logic_error` otherwise, and leaves it to the handler. `open()` sends the head at once, so that a client sees the stream before there is anything to
say; otherwise it leaves with the first event. Like the `response_writer`, it does not outlive the
handler: a stream that others feed is a handler waiting for what they post, which it writes itself.

`http::event_reader` reads them on the client, natively and in a browser alike, as a streamed `fetch`
there, which takes headers where `EventSource` does not:

```cpp
auto response = co_await client.stream(request);

if (response.status != http::status::ok || !http::is_event_stream(response.fields)) {
  co_return;   // a login page answering 200, say
}

http::event_reader events{std::move(response.body)};

while (auto const event = co_await events.next()) {
  show(event->type, event->data);
}
```

An event has a `type`, `"message"` unless the stream names another, its `data`, the lines of its `data`
fields joined with LF, and an `id`, the last event ID the stream set, empty once it reset it. When the stream ends, `next()`
gives nothing, and `events.last_event_id()`, the ID of the last event that ended, is what to send as
`Last-Event-ID` when connecting again, so that an event cut short comes again. `events.retry()` gives how
long the stream asked to wait first, at most a day. The reader does not connect again on its own.

| When                                      | What happens                                                                                                                                                                                                                                                                                                                                       |
| ----------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A stream has nothing to say for a while   | A client's `response_timeout` applies to each piece it waits for, 60 seconds by default: the server's `comment()` every 15 seconds or so keeps it, and a proxy, from closing the stream. It comes from the loop that sends the events, waiting for a change with a timeout: an operation asked while another is pending throws `std::logic_error`. |
| Lines end with CRLF, LF or CR             | All the same, even split between two pieces, and a BOM at the start is skipped.                                                                                                                                                                                                                                                                    |
| Text is not UTF-8                         | Read with U+FFFD in its place, as a browser does. `send()` throws `std::invalid_argument` for it.                                                                                                                                                                                                                                                  |
| An event is larger than the reader holds  | 1 MiB by default, its data and type, or its data with the line being read: `next()` throws `http::error::event_too_large`, and goes on throwing. An id longer than 1 KiB once read is ignored: the last ID stays, and the stream goes on.                                                                                                          |
| The stream ends in the middle of an event | That event is not one, as for a browser.                                                                                                                                                                                                                                                                                                           |
| A type or an id holds a line break        | `send()` throws `std::invalid_argument`, before anything leaves, as the line would end there. So does an id holding a NUL, or longer than 1 KiB, and a `comment()` with a line break or text that is not UTF-8.                                                                                                                                    |
| The server is stopped                     | It waits for a stream as for any request under way, up to `shutdown_timeout`, then stops what the handler awaits: a stream waiting for changes ends then.                                                                                                                                                                                          |
| The peer is gone                          | `send()` throws, as `response_writer::write()` does, which ends the handler.                                                                                                                                                                                                                                                                       |

Underneath, `http::format_event`, `format_retry` and `format_comment` write the text of an event, and
`http::event_parser` reads it as its pieces come, with no IO: for a transport of one's own.

## WebSocket

A WebSocket is a connection that switched from HTTP: a server accepts it in a route with
`http::accept_websocket`, whose handler then has the connection, as an `http::websocket`, for as long as
its task runs:

```cpp
router.get("/echo", [](http::request request) {
  return http::accept_websocket(request, [](http::websocket socket) -> async::task<> {
    for (;;) {
      auto message = co_await socket.receive();

      if (message.opcode == http::websocket_opcode::close) {
        co_return;
      }

      co_await socket.send_text(message.data);
    }
  });
});
```

The route answers 101, with the `Sec-WebSocket-Accept` the handshake asks for. A request that is no
handshake gets 400, or 426 with `Sec-WebSocket-Version: 13` for another version, and the handler never
runs. The response is the route's to complete, with a subprotocol from those the request offers in
`Sec-WebSocket-Protocol`, say. A route that refuses a handshake, for want of credentials say, returns a
response of its own instead. A browser sends its cookies with a handshake whatever page asks for it, and
holds no WebSocket to the same origin: a route that knows its users by a cookie checks the request's
`Origin` against its own before accepting, or a page of another site acts for them (cross-site WebSocket
hijacking). Over TLS, as `wss://`, it is the same: the handshake is no cryptography.

`receive()` gives each text or binary message, its fragments joined, then the peer's close, the last
message, with its `close_code` and reason in `data`. It answers pings with pongs, drops pongs, and answers
the close with its own: a loop that receives is all a connection needs to stay alive. `send_text()`,
`send_binary()`, `ping()` and `close()` go on while a `receive()` waits, from a task beside it through
`when_all`, each holding a copy of the `http::websocket` if it likes, as copies are the same connection, each message whole, in the order their tasks start. All of them run on the connection's own
strand, where the handler runs: a socket is not shared between threads, so that what another connection
wants sent, to broadcast say, goes to this handler, which sends it itself.

| When                         | What happens                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| ---------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| The peer breaks the protocol | It is sent the close the failure calls for: 1002, 1007 for text that is not UTF-8, 1009 for a message over `websocket_options::max_message_size`, 1 MiB by default. `receive()` throws the `http::error`, and goes on throwing.                                                                                                                                                                                                                                              |
| A close has gone out         | A message sent after it throws `http::error::websocket_closed`, and a `close()` does nothing. So does a message after a frame cut short, by a stop or a failure of the stream.                                                                                                                                                                                                                                                                                               |
| Two `receive()` at once      | The second throws `std::logic_error` when it is asked.                                                                                                                                                                                                                                                                                                                                                                                                                       |
| The peer stays quiet         | The connection's timeouts end with the handshake. After `websocket_options::ping_interval` with nothing read, 30 seconds by default, the peer is pinged; after another, it is taken as gone, or half-open, and the handler is stopped, which frees the connection. Only what `receive()` reads counts: a handler keeps receiving beside what else it does. A write that waits as long with nothing taken has the peer taken as gone: the send throws `std::errc::timed_out`. |
| The server is stopped        | As it starts to drain, a close with 1001 (going away) is sent: the peer's answer comes to `receive()` as the last message. A handshake that comes then gets 503. The server waits for the handler as for any request under way, up to `shutdown_timeout`, then stops what it awaits.                                                                                                                                                                                         |
| The handler ends             | A close goes, unless one did: 1000, or 1011 if it threw; then the server shuts its side. What it threw goes to `connection_options::on_handler_error`, but for what a stop throws, and a send refused after a close. A peer gone, or breaking the protocol, makes `receive()` throw: a handler catches it if that is no failure of its own.                                                                                                                                  |
| The peer is gone             | `receive()` throws the stream's error, and so do the sends that find it gone.                                                                                                                                                                                                                                                                                                                                                                                                |

Pinging and the 1001 of a drain need a timer, which `quoll::runtime` has.

Underneath, `response::upgrade`, an `http::upgrade_handler`, is what takes a connection over once a 101 has left,
which the server runs only for a request that asked to switch: any other response with it is replaced with a 500. `http::websocket_parser`,
`format_websocket_frame` and `format_websocket_close` read and write frames with no IO, for a transport of one's own.
A browser WebSocket is not there yet.

A client opens one with `http::connect_websocket`, on a connection of its own, natively:

```cpp
http::websocket_connect_options options;

options.fields.set("Sec-WebSocket-Protocol", "chat");

auto [socket, handshake] = co_await http::connect_websocket("wss://chat.example.com/room?id=4", options);

co_await socket.send_text("hello");

auto const answer = co_await socket.receive();
```

It goes through the network of the executor, as `quoll::runtime` provides it, or through a connector
given first, a memory stream in a test say. The server's 101 is checked, its `Sec-WebSocket-Accept`, no
extension, no subprotocol but one offered, and what it chose is in `handshake.fields`; any other answer
throws `http::websocket_refused`, whose `response()` it is. The connection and the handshake take no
longer than `options.handshake_timeout`, 30 seconds by default. A client's end masks what it sends, and
keeps itself alive while its `receive()`s read, as a server does: while one reads, a server that
sent nothing for `options.websocket.ping_interval`, 30 seconds by default, is pinged, and one that sent
nothing for another is taken as gone. The `receive()` then throws `std::errc::timed_out`, or the failure
of the ping that could not leave, and receiving has ended. So a client that only receives finds a server
that vanished without a word, a network cut or a host down; zero leaves that to the application.

## In a test

`http::local_transport` has no `stream()`. It runs a `request.body_writer` to its end before the handler,
which finds what it wrote in `request.body`, or as one piece through its `body_reader`. What a writer on a
response writes is gathered into `response.body`.
