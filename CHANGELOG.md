# Changelog

The notable changes of each version of quoll. The format follows [Keep a Changelog](https://keepachangelog.com/).

quoll follows [semantic versioning](https://semver.org/). While its major version is 0:

- a minor version, 0.9.0 to 0.10.0, may break the API;
- a patch version, 0.9.0 to 0.9.1, never does;
- `rust-version`, the oldest Rust that builds quoll, is raised only in a minor version.

quoll is used from git, at a version's tag:

```toml
[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll", tag = "v0.10.0" }
```

## [Unreleased]

### Added

- `cargo quoll dev` gives a server `env`, a table of variables set for it alone on top of its own environment, and
  `args`, the arguments it is started with, from its entry of `[[workspace.metadata.quoll.server]]`, such as
  `env = { HOST = "127.0.0.1", PORT = "3011" }` ([#138](https://github.com/4thlabs/quoll/issues/138)). A server
  restarted after a change gets them again. `[workspace.metadata.quoll.web]` takes an `env` too, set for `dev` alone.
  Both are checked when the config is read: a value that is not a string, a port written as a number included, and a
  variable or an argument that the OS cannot set are refused with the key at fault.
- An error carries headers, which the server sends with its status and its problem
  ([#139](https://github.com/4thlabs/quoll/issues/139)): `error.retry_after(Duration::from_secs(30))` on a 429 or a
  503 sets `Retry-After: 30`, and `error.with_header(name, value)` adds any other, a `WWW-Authenticate` on a 401 or an
  `Allow` on a 405. A `WWW-Authenticate` on a 401 takes the place of the route's challenges. `error.headers()` gives
  them back on every shape, and on the client the headers of any answer of an error status, whatever its body, so
  that `client::retry` and the caller read the wait the server asked. An internal error and an error of another API
  are answered 500 and 502 without them. A hop-by-hop header and the headers of the body quoll writes,
  `Content-Type`, `Content-Length`, `Content-Encoding` and `Content-Range`, are refused: the error is answered
  without them, and the log says which at `WARN`. The OpenAPI document does not change.
- `server::cors` exposes `Retry-After` and `Allow` too, so that a page of another origin reads them.

- A handler gives its problem the RFC 9457 `type` and extension members of its own, so that a client tells two errors
  of one status apart without reading the `detail` ([#140](https://github.com/4thlabs/quoll/issues/140)):
  `Error::new(StatusCode::CONFLICT, "alpha is running").with_type("/problems/app-running").with_member("app", "alpha")`.
  The problem of the answer and the `event: problem` that ends an `Events` stream write them after the RFC's own
  members, `into_shape()` keeps them, and the client reads them with `Problem::kind()` and `body_as`. A `type` is a
  URI reference, absolute or relative. A member is named as RFC 9457 has it, a letter, then letters, digits or `_`,
  three characters at least, and none of `type`, `title`, `status`, `detail` and `instance`. A refused `type` or
  member is left out and logged at `WARN`, never its value, and the error is answered as it is.
  `error.into_answered()` gives a fake the error that the client reads for a handler's error: its problem as its
  `detail()` and the problem's JSON as its `body()`. The OpenAPI document is unchanged.

### Changed

- The debug form of an error names its headers, never their values, and a header it was refused. That of an internal
  error is `Internal { source: .. }`, where it was `Internal(..)`.

### Fixed

- `.page(dir)` redirected every directory of the page asked without its final `/`, Vite's `assets/` among them, to a
  path with no file either. Only a directory that holds an `index.html`, `/docs`, is redirected now; any other is no
  file, with or without its final `/`: the page's `index.html` for a browser, a problem 404 otherwise. An
  application's layer that removes a final `/` in front of the page no longer loops on such a directory
  ([#137](https://github.com/4thlabs/quoll/issues/137)).

## [0.10.0] - 2026-11-25

### Added

- `client::follow(async move || api.feed().changes(&topic).await)` follows a stream route: it calls it at once, then
  again each time its stream ends or fails, after a wait ([#127](https://github.com/4thlabs/quoll/issues/127)). Each
  wait is drawn between half a bound and the bound, which doubles from 1 s to 30 s, and starts again from 1 s once a
  connection has stayed open 10 s; `client::Reconnect` sets `first`, `longest` and `steady`, and `without_jitter()`
  makes each wait its bound for a test. The stream gives `client::Followed` states: `Connected`, after each
  connection, `Item(T)`, `Skipped(error)` for an item that does not read, `Reconnecting { error, wait }` and
  `Stopped(error)`, the last. A 4xx other than 408 and 429, a 3xx and an `Invalid` call stop it; any other failure of
  the call reconnects, and so does any error of the stream but an item that does not read, an `event: problem`
  included. A fake of the group is followed as the client is.
- Every error has a kind, `error.kind()`, a `quoll::ErrorKind`: `Status(StatusCode)` for an error of a status, a
  handler's or another API's answer, and for a call of the client that got no answer to read, `Connect`, `Timeout`,
  `Closed`, `Body`, `Decode`, `Middleware` or `Invalid`. One `match` tells a refused connection from a deadline passed,
  or an item of a stream that does not read from a stream cut short, without reqwest's types. `status()` stays, and is
  `Some(status)` exactly when the kind is `Status(status)`. `ErrorKind::as_str()` and its `Display` give `404` or
  `timeout`, and `answered()` says whether the other API answered
  ([#126](https://github.com/4thlabs/quoll/issues/126)). A failure of reqwest is sorted by reqwest's own predicates,
  in this order: `is_builder` is `Invalid`, `is_connect` `Connect`, `is_timeout` `Timeout`, then `is_decode`,
  `is_redirect`, `is_upgrade` and an error of a `status` `Middleware`, a decision or a read by a middleware or by
  reqwest itself, and any other, `is_request` among them, `Closed`: the request left and no usable head came back. A
  body quoll reads that fails is `Body`; a middleware's own read of a body or `json()`, which reqwest reports as a
  decode error, is `Middleware`, and `Timeout` when it times out. A middleware's own error is `Middleware`, unless
  it passes on a reqwest error or an error of reqwest-middleware's, which keeps its kind whichever client it came
  from: a middleware whose own failure must read `Middleware` wraps it in an error type of its own. A middleware that
  turns an answer into an error with `error_for_status` fails the call as `Middleware`, with no status: the error's
  message says that a middleware failed with a reqwest error of that status, which says nothing of which client's
  answer it was.
- `Error::failed(kind, message)` builds a failure of the client of any kind, for a fake: a call that got no answer,
  whose `.response()` is the error, or with `ErrorKind::Status(status)` an answer of that status whose body is not the
  api's error shape, whose `.response()` is an answer of that status. A handler that returns it answers 502, or 500
  for `Invalid`.
- `client::Attempt::kind()` gives the kind of an attempt to a rule of `retry(..).when`: `None` for an answer of a
  success status, `Status(status)` for any other answer.
- `#[server(none)]` and `#[client(none)]` on an api, or on the group of a `quoll::group!`, say which side a contract
  does not have, whatever the features of `quoll`. Cargo builds `quoll` once per binary, with every feature its
  dependents ask for, so an application that serves its own api and calls another compiled the other's contract with a
  server too. `#[server(none)]`, for an API that is only called, leaves out the server side and the OpenAPI document;
  `#[client(none)]`, for one that is only served, such as a webhook receiver, leaves out the client side. The types
  then need only what the other side reads and writes. An api lists only groups that have each side it has, with a
  message that says which side a group lacks. The api reaches the client and the service of a group it lists through
  the group's descriptor, `ApiClient::notes` returning `<__QuollGroupNotes as Called>::Client` and `Api::router`
  taking a type parameter for its service; their docs link `notes::NotesClient` and `notes::NotesServer`. The groups
  of the api's block are named as before. The guides' contracts of other APIs and the Docker example say `#[server(none)]`
  ([#129](https://github.com/4thlabs/quoll/issues/129)).
- A route `-> quoll::TcpTunnel` opens a raw tunnel on its connection, as the Docker Engine API does for `exec` and
  `attach` ([#124](https://github.com/4thlabs/quoll/issues/124)). Its request sends `Connection: Upgrade` and
  `Upgrade: tcp`, through every middleware of the client, and the call gives the connection once the other api has
  answered `101 Switching Protocols` with `Upgrade: tcp`, over TCP or a Unix socket. The tunnel reads and writes with
  tokio's `AsyncRead` and `AsyncWrite`, `shutdown()` closes the half it writes, and `content_type()` gives the
  content type of the 101. The route compiles only in a contract with `#[server(none)]`: quoll's server does not open
  a tunnel yet. A 2xx in place of the 101, or a 101 to another protocol, is an error of the kind `Decode`, a 101 whose
  connection cannot be taken `Closed`, and an answer in HTTP/2 `Invalid`. A fake returns `TcpTunnel::new(io)`. The
  Docker example runs a command in a container with `docker-cli exec`.
- `server::Builder::send_timeout(duration)` and `without_send_timeout()`: a connection is cut once the bound, 60 s
  unless it is set, has passed with no write of an answer that its socket takes, as nginx's `send_timeout`
  ([#128](https://github.com/4thlabs/quoll/issues/128)). The clock runs only while a write waits, and stops when the
  socket takes bytes again. The kernel says there is room in large steps, once a good part of its send queue has
  drained: a reader is kept when it frees a good part of the queue within the bound. A quiet stream runs no clock.
  HTTP/1 connections and upgraded ones are bounded, which is what quoll serves; a stream of HTTP/2 is not. The
  handler's stream is dropped, as on a disconnection, a tokio `TcpStream` is reset so that the kernel frees its
  buffers at once, and each cut is logged once at `INFO` with the client's address.

### Changed

- `serve`, `serve_on` and `test::spawn` cut a connection whose socket takes no write of an answer for 60 s, where a
  client that stopped reading held its connection and the handler's stream for ever. `.without_send_timeout()` keeps
  them open as before. A router served with `axum::serve` has no bound. quoll needs tokio 1.50 or later.
- `client::trace` records `error.type` on its span, as OpenTelemetry names it: the kind of a call that failed,
  `connect`, `timeout`..., and the code of an answer of a 4xx or 5xx status, `404`.
- An `event: problem` whose data is not a problem is an item that does not read, of the kind `Decode`: the stream goes
  on after it, where it ended before. After an error, a stream of the client goes on when its kind is `Decode`, and
  has ended after any other.
- `client::retry` tries an idempotent request again after every failure of the kind `Closed`, any failure of reqwest
  that its other predicates do not name: a connection closed or reset before the answer, a broken pipe on a pooled
  connection that a proxy dropped, a request dropped by its connection, and a peer that answers bytes that are not
  HTTP. It tried again before, among the errors sent as requests, a connection reset or aborted, an incomplete answer
  and a cancelled request alone. A middleware's own error is not tried again, whatever it wraps, as its kind is
  `Middleware`.
- rustc's messages name quoll's `Status` type `quoll::Status`, by its path, since the variant `ErrorKind::Status`
  shares its name.

### Fixed

- A contract with no OpenAPI document, built with neither feature of `quoll`, left unused the import of a type that
  only the contract names, such as `use std::net::IpAddr;` for an argument: the macro names each type of its arguments
  and results whatever the features ([#129](https://github.com/4thlabs/quoll/issues/129)).

## [0.9.2] - 2026-11-24

### Added

- A `&` on an argument's type makes the client borrow it, so that a call needs no `.into()`, and a body or a list no
  `.clone()`: `list(owner: &str, #[join(",")] labels: &[&str], state: Option<&str>)` is called as
  `list("rust-lang", &["C-bug"], Some("open"))`, and `create(note: &NewNote)` as `create(&note)`. The server and the
  OpenAPI document receive the owned type, `String`, `Vec<String>`, `Option<String>`, `NewNote`. An argument with no
  `&` is taken by value, as before. `&Form<T>`, `&Text`, `&Bytes`, `&[String]` and `query filter: &NoteFilter` are
  borrowed too. With the server, `&mut X`, `&&X`, `&dyn Trait`, `&impl Trait` and a `&X` whose `X` has no size, as
  `&Path`, do not compile; a contract with a client alone takes them as before. The docs and the examples write
  `&str` for a string of the path, the query or a header ([#119](https://github.com/4thlabs/quoll/issues/119)).

## [0.9.1] - 2026-11-24

### Added

- `ByteStream::with_length` sends a stream of a known size, such as a file, with its `Content-Length` rather than in
  chunked encoding. A stream that gives another number of bytes is cut before its last bytes and logged, and the
  client reads an error. `ByteStream::length` gives the length on both sides, and on the client the `Content-Length`
  of the answer ([#115](https://github.com/4thlabs/quoll/issues/115)).

### Changed

- `server::builder()`'s `serve` and `serve_on`, and `test::spawn`, set `TCP_NODELAY` on every connection whose stream
  is tokio's `TcpStream`, so that a small write, an event of a stream or a chunk of a `ByteStream`, is no longer held
  for about 40 ms by Nagle's algorithm ([#114](https://github.com/4thlabs/quoll/issues/114)).

## [0.9.0] - 2026-11-23

The first release. One contract gives an axum server, a reqwest client and an OpenAPI 3.1 document. `cargo quoll`
keeps a frontend in sync with it. It needs Rust 1.91 or later.

### Added

- **The contract**: `quoll::api!`, a route table checked at compile time.
  - Methods, paths and queries, read by name: path and query parameters, query structs, headers and a body in JSON,
    `Form`, `Text` or `Bytes`.
  - Results with their status: a value, `Created`, `Accepted`, no content, and the streams `Events`, `JsonLines` and
    `ByteStream`.
  - Groups, each with its own trait, and large apis split across modules with `quoll::group!`.
  - Authentication schemes: `Bearer`, `Basic`, and keys in a header, a cookie or the query, with alternatives and
    optional credentials.
  - Fixed headers with `#[header]`, and fixed fields of a body with `#[body]`.
  - Errors as RFC 9457 problems, or in the shape of another API's errors with `#[error(E)]`.
  - A mistake is a compile error on the token at fault, with a message that says what to write instead.
- **The server**, behind the feature `server`.
  - One trait per group, served on an `axum::Router` by `server::builder()`, until Ctrl-C or `SIGTERM`.
  - `Authenticate<S>` turns a credential into the caller each handler receives.
  - Layers: `request_id`, `trace`, `cors`, `compression`, `timeout` and `body_limit`, put in the order they work in.
  - `quoll::test` serves a router on a port of its own, for a test. It comes with the feature `server`, and a test
    that calls the router with the real client also needs the feature `client`.
- **The client**, behind the feature `client`.
  - One method per route, built by `client::builder()` on reqwest-middleware.
  - Middleware: credentials, tokens that expire, `retry`, `timeout`, `redirects`, `trace`, `request_id`,
    `user_agent` and `on_request`.
  - A fake of each group, for the tests of code that calls an api.
  - Contracts of APIs that quoll did not write, such as the Docker Engine API, over TCP or a Unix socket.
- **The OpenAPI document**: `Api::openapi()` gives the OpenAPI 3.1 document of a contract, with the schemas of its
  types and its `///` comments.
- **`cargo quoll`**, the crate `quoll-cli`.
  - `cargo quoll gen` writes the document and runs the generator the workspace names: orval, openapi-typescript or
    any other. `gen --check` fails when a committed document is out of date.
  - `cargo quoll build` runs `gen`, then the page's build.
  - `cargo quoll dev` runs the servers, the generator and the page's dev server. When a file changes, it writes the
    document again and restarts the servers it touches.
  - Its config is in the workspace's `Cargo.toml`, under `[workspace.metadata.quoll]`.
- **Serving the page**: `server::builder().page(dir)` serves a built page beside the api, on one origin. A page's
  router gets `index.html` for its paths, and the files Vite names by their hash are cached for a year.

[Unreleased]: https://github.com/4thlabs/quoll/compare/v0.10.0...HEAD
[0.10.0]: https://github.com/4thlabs/quoll/compare/v0.9.2...v0.10.0
[0.9.2]: https://github.com/4thlabs/quoll/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/4thlabs/quoll/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/4thlabs/quoll/releases/tag/v0.9.0
