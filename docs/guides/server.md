# The server

With the feature `server`, a contract gives one trait per group, which a service implements. It also gives
`Api::router`, which builds the `axum::Router` of the api. `quoll::server::builder()` serves that router with its
layers.

## A server

```rust,no_run
use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use http::header::LOCATION;
use quoll::{Created, Error, Response, Result, server};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
}

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

/// The notes, and the id of the next one: never one a note had, removed or not.
#[derive(Debug, Default)]
struct Store {
    last_id: i64,
    notes: BTreeMap<i64, Note>,
}

/// The service: its state is its own fields, shared by its clones.
#[derive(Debug, Clone, Default)]
struct App {
    store: Arc<Mutex<Store>>,
}

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<Note>> {
        let store = self.store.lock().expect("no handler panics while holding the lock");

        store
            .notes
            .get(&id)
            .cloned()
            .map(Response::new)
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Response<Note>> {
        let mut store = self.store.lock().expect("no handler panics while holding the lock");

        store.last_id += 1;

        let note = Note {
            id: store.last_id,
            title: note.title,
        };
        let location = format!("/api/notes/{}", note.id);

        store.notes.insert(note.id, note.clone());
        Ok(Response::new(note).header(LOCATION, location))
    }

    async fn remove(&self, id: i64) -> Result<Response<()>> {
        let mut store = self.store.lock().expect("no handler panics while holding the lock");

        store.notes.remove(&id);
        Ok(Response::new(()))
    }
}

#[tokio::main]
async fn main() -> std::io::Result<()> {
    tracing_subscriber::fmt::init();

    let router = Api::router(ApiServices {
        notes: App::default(),
    });

    server::builder()
        .with(server::request_id())
        .with(server::trace())
        .with(server::timeout(Duration::from_secs(10)))
        .with(server::body_limit(1 << 20))
        .serve("0.0.0.0:8080", router)
        .await
}
```

## Services

- A group `Notes` gives the trait `NotesServer`, with one handler per route. An implementation writes `async fn`.
- A handler takes the route's arguments in the contract's order. It returns a `quoll::Result` of a `quoll::Response`
  of the route's result: `Ok(Response::new(note))`. For a `Created<T>` or an `Accepted<T>`, that result is the `T`,
  and the contract gives the status. The response carries the headers that the handler sends,
  `.header(LOCATION, location)`: [Responses](#responses) says how.
- A handler of a route that takes a credential receives the caller after `&self`: see
  [Authentication](authentication.md).
- One type may implement the traits of several groups.
- A service is `Send + Sync + 'static`. The router holds it in an `Arc`, and its state is its own fields: there is no
  axum `State`.

`ApiServices` has one field per group, named after it in snake case: `notes` for `Notes`. A field left out does not
compile, and neither does a service that does not implement its group's trait:

```rust
use quoll::{Response, Result};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list() -> Vec<String>;
    }

    pub group Admin {
        GET "/admin/stats" => stats() -> u64;
    }
}

/// One type serves both groups.
#[derive(Debug, Clone)]
struct App;

impl NotesServer for App {
    async fn list(&self) -> Result<Response<Vec<String>>> {
        Ok(Response::new(vec!["Milk".into()]))
    }
}

impl AdminServer for App {
    async fn stats(&self) -> Result<Response<u64>> {
        Ok(Response::new(1))
    }
}

let router = Api::router(ApiServices {
    notes: App,
    admin: App,
});
```

## Responses

A handler returns a `quoll::Response` of the route's value: `Note` for `-> Note`, `-> Created<Note>` and
`-> Accepted<Note>`, `()` for a route with no result, and the stream for a stream.

- `Response::new(value)` answers the status the contract says: 200, 201 for `Created<T>`, 202 for `Accepted<T>`, 204
  for no result.
- `.header(name, value)` adds a header, after those of the same name: an `ETag`, a `Location`, a `Cache-Control`. A
  handler's header replaces a header of that name that quoll puts, such as a stream's `Cache-Control`.
- `.with_status(Status::...)` answers another success or a redirection in place of the contract's status. For
  example: a `PUT` that answers 201 when it creates and 200 when it replaces, a 304 Not Modified, or a 206.
  `quoll::Status` holds only a `2xx` or a `3xx`, so an error's status does not compile there. An error is an
  `Err(quoll::Error)`: see [Errors](errors.md).
- `Status::NO_CONTENT` and `Status::RESET_CONTENT`, a 204 and a 205, say there is no body. They go on a value that
  may be empty:
  - `()`, an `Option<T>` such as `Response::new(None)`, a `Text`, `Bytes` or a stream;
  - the `()` or the `Option<T>` of a `Created` or an `Accepted`, which answers JSON: `-> Accepted<()>` or
    `-> Created<Option<Note>>`.

  The client reads them as `()`, `None`, empty, or a stream that ends at once. On a value that must be there, such as
  the `Note` of `-> Note`, `-> Created<Note>` or `-> Accepted<Note>`, they do not compile: write an `Option<Note>`.
- `Status::NO_CONTENT` and `Status::RESET_CONTENT` are constants of type `quoll::NoContent`, not `Status`, so that
  one `if` cannot pick between a 201 and a 204. A handler that answers either one, depending on whether it creates or
  replaces, calls `with_status` in each branch. `NoContent::try_from(status)` gives a `NoContent` from the status of
  another API's answer.
- A 204, a 205 or a 304 is sent without a body, because HTTP has none for them: the value of the `Response` is not
  sent.
- quoll answers 500 for a name or a value that is not valid in a header, and logs it without the value. It does the
  same for a `Content-Type`, a `Content-Length` or a `Transfer-Encoding`, which quoll writes from the route's
  result. A `ByteStream` gives its length with `with_length`: see [A length](contract.md#a-length).

The OpenAPI document describes the contract's status and none of these headers.

```rust
use http::header::{ETAG, LOCATION};
use quoll::{Created, Response, Result, Status};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{id}" => get(id: i64, header("If-None-Match") seen: Option<&str>) -> String;
        PUT  "/notes/{id}" => put(id: i64, title: String) -> String;
        POST "/notes"      => create(title: String) -> Created<i64>;
    }
}

#[derive(Debug, Default)]
struct App;

impl NotesServer for App {
    async fn get(&self, id: i64, seen: Option<String>) -> Result<Response<String>> {
        let tag = format!("\"{id}\"");
        let response = Response::new("Milk".to_owned()).header(ETAG, tag.as_str());

        if seen.as_deref() == Some(tag.as_str()) {
            return Ok(response.with_status(Status::NOT_MODIFIED));
        }

        Ok(response)
    }

    async fn put(&self, id: i64, title: String) -> Result<Response<String>> {
        let response = Response::new(title);

        if id > 100 {
            return Ok(response.with_status(Status::CREATED));
        }

        Ok(response)
    }

    async fn create(&self, _title: String) -> Result<Response<i64>> {
        let id = 7;

        Ok(Response::new(id).header(LOCATION, format!("/api/notes/{id}")))
    }
}
# let _ = Api::router(ApiServices { notes: App });
```

## Layers

`server::builder()` takes quoll's layers, and any tower layer, with `with`:

| Layer                   | What it does                                                                         |
| ----------------------- | ------------------------------------------------------------------------------------ |
| `server::request_id()`  | keeps the caller's `X-Request-Id` or makes one, and sends it back                    |
| `server::trace()`       | a tracing span per request, with its operation and its id, and a line per answer     |
| `server::cors(origins)?`| CORS for the pages of the origins given                                              |
| `server::compression()` | gzip, brotli and zstd answers, as the caller accepts, but not streams               |
| `server::timeout(d)`    | a handler that takes longer than `d` answers 503                                     |
| `server::body_limit(n)` | a body larger than `n` bytes answers 413                                             |

Each one answers its errors as RFC 9457 problems, like handlers.

The builder puts quoll's layers in the order they work in, whatever order they are given in. From the outermost:

1. `request_id`;
2. `trace`;
3. `cors`;
4. `compression`;
5. `timeout`;
6. `body_limit`.

So a request gets its id, then its span. A 503 of `timeout` or a 413 of `body_limit` is traced, and carries the id and
the CORS headers. Any other layer keeps the place it is given, and the first one given is the outermost.

```rust
use quoll::server;

fn served(router: axum::Router) -> Result<axum::Router, server::InvalidOrigin> {
    let router = server::builder()
        .with(server::cors(["https://app.example.com", "http://localhost:5173"])?)
        .with(server::compression())
        .with(server::request_id())
        .into_router(router);

    Ok(router)
}
```

`cors` returns a `server::InvalidOrigin` for what is not an origin, such as `*` or `https://example.com/app`. It
allows no credentials: a page of another origin sends a token or a key in a header. Such a page reads `Location`,
`WWW-Authenticate`, `Retry-After`, `Allow` and `X-Request-Id` in an answer, beside the headers that every page
reads.

The docs of each helper, under `quoll::server`, say in full what it does: which ids `request_id` keeps, what `trace`
logs and at which level, and what `compression` leaves out.

### An application's own layer

Any tower layer goes to `with`, such as one of axum's `middleware::from_fn`. The answer of each route carries its
`quoll::Route`, which a layer reads:

```rust
use axum::extract::Request;
use axum::middleware::{self, Next};
use axum::response::Response;
use quoll::{Route, server};

/// Prints the operation of each answer that a route made.
async fn log_operation(request: Request, next: Next) -> Response {
    let response = next.run(request).await;

    if let Some(route) = response.extensions().get::<Route>() {
        println!("{} answered {}", route.operation_id(), response.status());
    }

    response
}

fn served(router: axum::Router) -> axum::Router {
    server::builder()
        .with(server::request_id())
        .with(middleware::from_fn(log_operation))
        .into_router(router)
}
```

An answer that no route made carries no `Route`: the 404 of a path that is no route, the 405 of a route's path with
another method, a file of [the page](#the-page), a 503 of `timeout`, or an early 413 of `body_limit`.

### The request id

A handler reads the id of its request with `server::RequestId::current()`, to pass it on or log it beside an error.
It is `None` outside a handler's future: in a task the handler spawns, in the stream it returns, or with no
`request_id()` layer.

```rust
use quoll::server::RequestId;

fn request() -> String {
    RequestId::current().map_or_else(|| "none".to_owned(), |id| id.to_string())
}

assert_eq!(request(), "none");
```

## Serving

- `.serve(address, router)` binds an address and serves: `"0.0.0.0:8080"`, a `String`, or a `SocketAddr`.
- `.serve_on(listener, router)` serves a listener that is already bound: a `TcpListener` on port 0, or a
  `UnixListener`.
- `.into_router(router)` gives the router with its layers, for an application that serves it itself, or for
  [a test](testing.md).

`serve`, `serve_on` and [`test::spawn`](testing.md) set `TCP_NODELAY` on each connection whose stream is tokio's
`TcpStream`: a small write, such as an event of a stream, is sent at once. A listener whose stream wraps a TCP
stream, such as TLS, sets the option itself, on the accepted `TcpStream`. A router served otherwise, with
`axum::serve`, does not get it.

`serve` and `serve_on` return an `std::io::Result<()>`. It is an error when the address cannot be bound, or when the
signals cannot be listened to.

```rust,no_run
use quoll::server;

#[tokio::main]
async fn main() -> std::io::Result<()> {
    let router = axum::Router::new();
    let listener = tokio::net::UnixListener::bind("/run/notes/api.sock")?;

    server::builder()
        .with(server::trace())
        .serve_on(listener, router)
        .await
}
```

### A client that stops reading

`serve`, `serve_on` and `test::spawn` cut a connection once 60 s have passed with no write of an answer that its
socket takes, as nginx's `send_timeout` does. Without it, a client that stops reading, such as a stalled tab or
proxy, would hold its connection and the handler's stream for ever.

- The clock runs only while a write waits. It starts once the kernel's buffers are full, and stops when the socket
  takes bytes again.
- The kernel says there is room in large steps, once a good part of its send queue has drained, not at each byte the
  client reads. A reader is kept when it frees a good part of the queue within the bound. On a real link the queue
  follows the link's rate and stays small. On loopback it can grow to several MiB.
- A stream with nothing to send runs no clock. The keep-alive comments of an `Events` are too small to fill the
  buffers.
- Every answer of an HTTP/1 connection is bounded, which is what quoll serves: its head, a large body and a stream
  alike, and an upgraded connection. A stream of HTTP/2, which an application gets by turning on axum's `http2`, is
  not bounded: it waits on its flow-control window, not on the socket.
- The cut ends the connection, as a disconnection does. The handler's stream is dropped at its next `.await`, and the
  client reads an error. A tokio `TcpStream` is reset, so that the kernel frees what it holds for the client at once.
- Each cut is logged once at `INFO`, with the client's address.

`.send_timeout(duration)` sets the bound, and `.without_send_timeout()` leaves such a connection open. A router served
with `axum::serve` has no bound.

```rust,no_run
use std::time::Duration;

use quoll::server;

#[tokio::main]
async fn main() -> std::io::Result<()> {
    let router = axum::Router::new();

    server::builder()
        .send_timeout(Duration::from_secs(30))
        .serve("0.0.0.0:8080", router)
        .await
}
```

## The page

`.page(dir)` serves the page that `cargo quoll build` bundles, from its directory. The page and the api then share
one origin. The page calls `/api/...` with no CORS, and a cookie scheme works as it is.

```rust,no_run
use quoll::server;

#[tokio::main]
async fn main() -> std::io::Result<()> {
    let router = axum::Router::new();

    server::builder()
        .page("web/dist")
        .serve("0.0.0.0:8080", router)
        .await
}
```

The server reads the directory at each request. A relative path is relative to the directory the server runs in. A
new page is deployed without a new build of the server. `serve` and `serve_on` refuse to start when the directory has
no `index.html`. `.page(dir)` replaces any fallback the router has.

The files are served by tower-http's `ServeDir`, as in axum's own example. quoll adds thin layers around it.

What the server answers, for a path outside the api's prefix:

- A file of that path in the directory is sent, with the content type of its extension. `/` sends `index.html`.
- A directory that holds an `index.html` sends it, as a page of a multi-page build does. `/docs` without its final `/`
  is a 307 to `/docs/`, with the same query.
- A directory that holds no `index.html`, as Vite's `assets/`, is no file, with or without its final `/`, and is never
  redirected: it is answered as the next two points say. An application's layer that removes a final `/` only has to
  leave alone the directories that hold an `index.html`.
- Otherwise, a `GET` or a `HEAD` whose `Accept` holds `text/html` gets `index.html`, with `Vary: Accept`. A browser
  sends that `Accept` when it loads a page, so the page's router shows the path: `/notes/3`, or `/users/jane.doe`
  after F5. A script, a style, an image or a `fetch` does not send it: `*/*` alone does not count.
- Everything else is a problem 404. A missing script is a 404, not `index.html` read as a script.
- Under `/.well-known/`, a missing file is always a 404: programs read those files, not browsers. The path is decoded
  first, so `/%2Ewell-known/` is the same directory.

A path under the api's prefix that is no route answers a problem 404. It never reaches the page. `HEAD` gets the
headers alone. A method other than `GET` and `HEAD` is a problem 405. A file the server cannot read, as one of the wrong
permissions, is no file: it gets `index.html` or a 404, as `ServeDir` does.

What the server never sends:

- A hidden file or directory, whose name begins with a `.`: `/.env`, `/.git/config` or `/.vite/manifest.json`. Only
  `/.well-known/` is served.
- A file outside the directory. A path with `..` answers a problem 404.
- A symbolic link whose target is outside the directory, or hidden. Before `ServeDir` reads a file, the server finds
  the file `ServeDir` will open, as `ServeDir` does: a directory's `index.html`, or the file of the path whatever its
  final `/`. It then checks the real path of that file and of each compressed copy of it, without opening them. A
  directory that is such a link is refused too, whatever it holds. A short name of Windows, `GIT~1`, is refused the
  same way. A link swapped in between that check and the read is not seen. Only someone who can write in the
  directory can do that.
- A path with an empty segment, `//evil.example`. `ServeDir` would redirect it to `//evil.example/`, which a browser
  reads as another host.
- A file whose time no HTTP date holds, before 1970 or from the year 10000. It answers a problem 500 that names it,
  as `ServeDir` cannot date it. Touch the file to fix it.

The headers of a file:

- `X-Content-Type-Options: nosniff` and `Vary: Accept-Encoding` are set on every answer of the page, its errors
  too.
- A file Vite names by its hash, like `assets/index-BXa3kD9f.js`, never changes under its name. It is
  `Cache-Control: public, max-age=31536000, immutable`. `index.html` and every other file are `no-cache`: the browser
  checks them again before each use.
- `Last-Modified` answers `If-Modified-Since` with a 304. `Range` gets a part of the file. There is no `ETag`.
- `index.html` is checked again by its `Last-Modified`, as every other file. A build that fixes the times of its
  files, with `SOURCE_DATE_EPOCH`, must give `index.html` a new time, `touch dist/index.html`. Otherwise a browser
  keeps the `index.html` of the last deployment, as the server answers its `If-Modified-Since` with a 304. A restart
  of the server does not change that.
- A compressed copy beside a file, `app.js.br`, `app.js.gz` or `app.js.zst`, is sent in its place when the request
  accepts its coding. Vite does not write these copies by default.

### Vite's manifest

Turn on `build.manifest` in `vite.config.ts`, as the notes example does:

```typescript
export default defineConfig({
  build: { manifest: true },
});
```

Vite then writes `.vite/manifest.json`, which names every file it hashed. A file is kept a year when its name has the
shape of a hash and the manifest names it, or names the file it is the `.map` of. The shape is a `-`, then eight
letters, digits, `-` or `_`, then a `.`. The server reads the manifest again when it changes, at each deployment. The
manifest is hidden, so it is never sent.

Without a manifest, the server guesses from the name alone. The file must be under `assets/`, and its eight
characters must hold at least one digit. A name written by hand, like `assets/logo-Darkmode.svg`, has no digit there
and is not taken. But about a quarter of Vite's hashes hold no digit either: without the manifest, those files are
`no-cache`, and the browser checks them again for nothing.

### The api's 404

The api's router answers a problem 404 for any path under its prefix that no route serves, with or without a page. A
path that a route serves keeps axum's 405 for another method. A path outside the prefix gets axum's 404 when there is
no page.

- An api with no prefix, `""`, has no such 404. Every path that is no route goes to the page.
- Two apis of the same prefix cannot be merged with `Router::merge`: axum panics on their two fallbacks. Give each
  api its own prefix. Two apis with different prefixes merge.
- A router nested with `Router::nest` loses the 404 of its prefix. Its unknown paths get the outer router's fallback.

### A release

A release is two things: the server's binary, from `cargo build --release`, and the page's directory, `web/dist`, from
`cargo quoll build`. Deploy both. Run the server from the directory that holds `web/dist`, or give `.page` an absolute
path.

The notes server serves its page when `NOTES_PAGE` names the directory: `NOTES_PAGE=web/dist`. `cargo quoll dev` does
not set it: Vite serves the page there, and proxies `/api` to the server.

## Shutdown

A server serves until Ctrl-C, or until `SIGTERM` on Unix, which `docker stop` and Kubernetes send. Once told to stop,
it accepts no more connections, and waits for the requests it serves to end. It waits 5 s, or what `.grace(duration)`
says, or until a second Ctrl-C or `SIGTERM`. Then it logs at `WARN` that connections were left open, and returns `Ok`.
Those connections end with the runtime, when `main` returns.

The streams in flight:

- An `Events` stream ends at once, after the event it is sending, with no `event: problem`. Its client, and any proxy
  between, reads a clean end, the same as when the handler's stream ends. An `EventSource` connects again after it:
  see [the TypeScript guide](typescript.md#events-with-a-cookie).
- A `JsonLines` or a `ByteStream` is not ended. It runs on until the grace has passed, and is then cut. Its client
  reads that as an error. A clean end would make a finite one with no length look complete. A `ByteStream` with a
  length is read as cut either way.

What a stream should say about a stop is an open question: [#70](https://github.com/4thlabs/quoll/issues/70). A router
served by `axum::serve` itself, or by `quoll::test::spawn`, leaves its streams to run until their connection closes.

`.shutdown(signal)` stops the server when a future ends, in place of the signals. The future may be a channel in a
test, or `std::future::pending()` for a server that is never told to stop.

```rust
use std::time::Duration;

use quoll::server;
use tokio::sync::oneshot;

#[tokio::main]
async fn main() -> std::io::Result<()> {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
    let (stop, stopped) = oneshot::channel::<()>();

    let served = tokio::spawn(
        server::builder()
            .shutdown(async move {
                let _ = stopped.await;
            })
            .grace(Duration::from_secs(1))
            .serve_on(listener, axum::Router::new()),
    );

    stop.send(()).expect("the server waits for the signal");
    served.await.expect("the server does not panic")
}
```

Once a server has listened to Ctrl-C and `SIGTERM`, tokio keeps handling them: the process does not get their default
action back when the server returns.

## Errors

A handler returns a `quoll::Error`. The server answers it with its status and an RFC 9457 problem, like
`Error::not_found("no such note")` above. `Error::internal(cause)` logs its cause and answers a bare 500.
[Errors](errors.md) says what each error answers.

An error carries headers, which the server sends with its status. `.retry_after(Duration::from_secs(30))` on a 429
answers `Retry-After: 30`, and `.with_header(name, value)` adds any other, like a `WWW-Authenticate` on a 401.
[Headers](errors.md#headers) says which headers are refused.
