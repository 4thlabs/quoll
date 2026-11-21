# The server

With the feature `server`, a contract gives a trait per group, which a service implements, and `Api::router`, which
gives the `axum::Router` of the api. `quoll::server::builder()` serves it with its layers.

## A server

```rust,no_run
use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use quoll::rest::{self, Created, Error, Result};
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
    async fn get(&self, id: i64) -> Result<Note> {
        let store = self.store.lock().expect("no handler panics while holding the lock");

        store
            .notes
            .get(&id)
            .cloned()
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Created<Note>> {
        let mut store = self.store.lock().expect("no handler panics while holding the lock");

        store.last_id += 1;

        let note = Note {
            id: store.last_id,
            title: note.title,
        };

        store.notes.insert(note.id, note.clone());
        Ok(Created::at(format!("/api/notes/{}", note.id), note))
    }

    async fn remove(&self, id: i64) -> Result<()> {
        let mut store = self.store.lock().expect("no handler panics while holding the lock");

        store.notes.remove(&id);
        Ok(())
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
- A handler takes the route's arguments in the contract's order, and returns a `rest::Result` of the route's result.
- A handler of a route that takes a credential receives the caller after `&self`: see
  [Authentication](authentication.md).
- One type may implement the traits of several groups.
- A service is `Send + Sync + 'static`. The router holds it in an `Arc`, and its state is its own fields: there is no
  axum `State`.

`ApiServices` has a field per group, named after it in snake case, `notes` for `Notes`. A field left out does not
compile, nor does a service that does not implement its group's trait:

```rust
use quoll::rest::{self, Result};

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
    async fn list(&self) -> Result<Vec<String>> {
        Ok(vec!["Milk".into()])
    }
}

impl AdminServer for App {
    async fn stats(&self) -> Result<u64> {
        Ok(1)
    }
}

let router = Api::router(ApiServices {
    notes: App,
    admin: App,
});
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
no more connections, and returns once the requests it serves have ended.

The `Events` streams in flight end at once, after the event each is sending, so that the client, and a proxy between,
reads a clean end, as when the handler's stream ends. They end with nothing more: a stop is no failure, and sends no
`event: problem`, so that such an end cannot be told from a finite stream that completes, an open question,
[#70](https://github.com/4thlabs/quoll/issues/70). An `EventSource` connects again after it, and goes on only when it
is answered with a 200: a proxy that answers a 5xx while the server restarts closes it for good. A router served by
`axum::serve` itself, or `quoll::test::spawn`, leaves them to run.

It waits 5 s for the other requests, or what `.grace(duration)` says, or until a second Ctrl-C or `SIGTERM`. Then it
logs at `WARN` that connections were left open, and returns `Ok`. They end with the runtime, as `main` returns.

A `JsonLines` or a `ByteStream` in flight is one of those requests: it is not ended, as a clean end would make a finite
one, an export or a file, cut by the stop look complete. It runs on until the grace has passed, and is cut with its
connection as the runtime ends, which its client reads as an error. What it should say of a stop is an open question,
[#70](https://github.com/4thlabs/quoll/issues/70).

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
