# The server

With the feature `server`, a contract gives a trait per group, which a service implements, and `Api::router`, which
gives the `axum::Router` of the api. `quoll::server::builder()` serves it with its layers.

## A server

```rust,no_run
use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use http::header::LOCATION;
use quoll::rest::{self, Created, Error, Response, Result};
use quoll::server;
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

rest::api! {
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

- A group `Notes` gives the trait `NotesServer`, with a handler per route. An implementation writes `async fn`.
- A handler takes the route's arguments in the contract's order, and returns a `rest::Result` of a `rest::Response` of
  the route's result, the `T` of a `Created<T>` or an `Accepted<T>`, whose status the contract gives:
  `Ok(Response::new(note))`, with the headers it sends, `.header(LOCATION, location)`, as [Responses](#responses)
  says.
- A handler of a route that takes a credential receives the caller after `&self`: see
  [Authentication](authentication.md).
- One type may implement the traits of several groups.
- A service is `Send + Sync + 'static`. The router holds it in an `Arc`, and its state is its own fields: there is no
  axum `State`.

`ApiServices` has a field per group, named after it in snake case, `notes` for `Notes`. A field left out does not
compile, nor does a service that does not implement its group's trait:

```rust
use quoll::rest::{self, Response, Result};

rest::api! {
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

A handler returns a `rest::Response` of the route's value: `Note` for `-> Note`, `-> Created<Note>` and
`-> Accepted<Note>`, `()` for a route with no result, the stream for a stream.

- `Response::new(value)` answers the status the contract says: 200, 201 for `Created<T>`, 202 for `Accepted<T>`, 204
  for no result.
- `.header(name, value)` adds a header, after those of its name: an `ETag`, a `Location`, a `Cache-Control`. A
  handler's header takes the place of one quoll puts of its name, as a stream's `Cache-Control`.
- `.with_status(Status::...)` answers another success or a redirection in place of the contract's: a `PUT` that
  answers 201 when it creates and 200 when it replaces, a 304 Not Modified, a 206. `rest::Status` holds a `2xx` or a
  `3xx` alone, so an error's status does not compile there: an error is an `Err(rest::Error)`, as
  [Errors](errors.md) says.
- `Status::NO_CONTENT` and `Status::RESET_CONTENT`, a 204 and a 205, say there is no body: they go on a value that
  may be empty, `()`, an `Option<T>`, `Response::new(None)`, a `Text`, `Bytes` or a stream, whatever the result wraps
  it in, an `Accepted<()>` or a `Created<Option<Note>>` too, and the client reads them as `()`, `None`, empty, or a
  stream that ends at once. On a value that must be one, the `Note` of `-> Note`, `-> Created<Note>` or
  `-> Accepted<Note>`, they do not compile: write an `Option<Note>`. `NoContent::try_from(status)` gives one back from
  another API's answer, and a route with no result that answers 201 or 204 gives each in its own branch.
- A 204, a 205 or a 304 is sent without its body, which HTTP gives them none: the value of the `Response` is not sent.
- A name or a value that is not a header's, or a `Content-Type`, a `Content-Length` or a `Transfer-Encoding`, which
  quoll writes from the route's result, answers 500, logged without the value.

The OpenAPI document describes the contract's status and none of these headers.

```rust
use http::header::{ETAG, LOCATION};
use quoll::rest::{self, Created, Response, Result, Status};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{id}" => get(id: i64, header("If-None-Match") seen: Option<String>) -> String;
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
| `server::compression()` | gzip, brotli and zstd answers, as the caller accepts, streams aside                  |
| `server::timeout(d)`    | a handler that takes longer than `d` answers 503                                     |
| `server::body_limit(n)` | a body larger than `n` bytes answers 413                                             |

Each answers its errors as RFC 9457 problems, as handlers do.

The builder puts quoll's layers in the order they work in, whatever the order they are given in, from the outermost:
`request_id`, `trace`, `cors`, `compression`, `timeout`, `body_limit`. So a request gets its id, then its span, and a
503 of `timeout` or a 413 of `body_limit` is traced, carries the id and the headers of CORS. Any other layer keeps the
place it is given at, the first given the outermost.

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

`cors` returns a `server::InvalidOrigin` for what is not an origin, as `*` or `https://example.com/app`. It allows no
credentials: a page of another origin sends a token or a key in a header.

The docs of each helper, under `quoll::server`, say what it does in full: which ids `request_id` keeps, what `trace`
logs and at which level, what `compression` leaves out.

### A layer of the application's

Any tower layer goes to `with`, as one of axum's `middleware::from_fn`. The answer of each route carries its
`rest::Route`, which a layer reads:

```rust
use axum::extract::Request;
use axum::middleware::{self, Next};
use axum::response::Response;
use quoll::rest::Route;
use quoll::server;

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

An answer no route made carries no `Route`: axum's 404 and 405, a 503 of `timeout`, an early 413 of `body_limit`.

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
- `.serve_on(listener, router)` serves a listener bound already: a `TcpListener` on port 0, or a `UnixListener`.
- `.into_router(router)` gives the router with its layers, for an application that serves it itself, or for
  [a test](testing.md).

`serve` and `serve_on` return an `std::io::Result<()>`: an error when the address cannot be bound, or the signals
cannot be listened to.

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

## Shutdown

A server serves until Ctrl-C, or `SIGTERM` on Unix, which `docker stop` and Kubernetes send. Told to stop, it accepts
no more connections, and waits for the requests it serves to end: 5 s, or what `.grace(duration)` says, or until a
second Ctrl-C or `SIGTERM`. Then it logs at `WARN` that connections were left open, and returns `Ok`: they end with
the runtime, as `main` returns.

The streams in flight:

- An `Events` stream ends at once, after the event it is sending, with no `event: problem`: its client, and a proxy
  between, reads a clean end, as when the handler's stream ends. An `EventSource` connects again after it, as
  [the TypeScript guide](typescript.md#events-with-a-cookie) says.
- A `JsonLines` or a `ByteStream` is not ended: it runs on until the grace has passed, and is then cut, which its
  client reads as an error.

What a stream should say of a stop is an open question, [#70](https://github.com/4thlabs/quoll/issues/70). A router
served by `axum::serve` itself, or by `quoll::test::spawn`, leaves its streams to run until their connection closes.

`.shutdown(signal)` stops it when a future ends, in the place of the signals: a channel of a test, or
`std::future::pending()` for a server that is never told to stop.

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

Once a server has listened to Ctrl-C and `SIGTERM`, they stay tokio's: the process does not get their default action
back when it returns.

## Errors

A handler returns a `rest::Error`, which the server answers with its status and an RFC 9457 problem, as
`Error::not_found("no such note")` above. `Error::internal(cause)` logs its cause and answers a bare 500.
[Errors](errors.md) says what each error answers.
