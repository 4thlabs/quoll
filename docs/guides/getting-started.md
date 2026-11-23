# Getting started

This guide builds a first contract, its server and its client, in one crate, and runs them.

## The crate

quoll is used from git until it is published. A new crate adds it with both features. It also needs the crates that a
contract uses: serde for the bodies, schemars for the OpenAPI document, and tokio to run.

```bash
cargo new notes
cd notes
```

`Cargo.toml`:

```toml
[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll", features = ["server", "client"] }
schemars = "1"
serde = { version = "1", features = ["derive"] }
tokio = { version = "1", features = ["macros", "rt-multi-thread"] }
```

## The code

`src/main.rs` holds the contract, its server and its client. `cargo run -- serve` serves it, `cargo run` calls it,
and `cargo run -- openapi` prints its OpenAPI document.

```rust,no_run
use std::sync::Mutex;

use quoll::{Created, Error, Response, Result, client, server};
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
    /// Notes, kept in memory.
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{id}" => get(id: i64) -> Note;
        POST "/notes"      => create(note: NewNote) -> Created<Note>;
    }
}

/// The server's state: its notes.
#[derive(Default)]
struct App {
    notes: Mutex<Vec<Note>>,
}

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<Note>> {
        let notes = self.notes.lock().expect("no handler panics while holding the lock");

        notes
            .iter()
            .find(|note| note.id == id)
            .cloned()
            .map(Response::new)
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Response<Note>> {
        let mut notes = self.notes.lock().expect("no handler panics while holding the lock");
        let note = Note {
            id: notes.len() as i64 + 1,
            title: note.title,
        };
        let location = format!("/api/notes/{}", note.id);

        notes.push(note.clone());
        Ok(Response::new(note).header("location", location))
    }
}

async fn serve() -> std::io::Result<()> {
    let router = Api::router(ApiServices {
        notes: App::default(),
    });

    server::builder().serve("127.0.0.1:8080", router).await
}

async fn call() -> quoll::Result<()> {
    let api = client::builder().build::<Api>("http://127.0.0.1:8080")?;

    let created = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
        })
        .await?;
    let read = api.notes().get(created.id).await?;

    println!("note {}: {}", read.id, read.title);
    Ok(())
}

#[tokio::main]
async fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    match std::env::args().nth(1).as_deref() {
        Some("serve") => serve().await?,
        Some("openapi") => println!("{}", Api::openapi().to_json_pretty()),
        _ => call().await?,
    }

    Ok(())
}
```

What each part does:

- **The models**, `Note` and `NewNote`, are serde types, with a JSON Schema for the document.
- **The contract**, `quoll::api!`, is a route table: `GET "/notes/{id}"` becomes a function `get(id: i64)`, which
  returns a `Note`. [The contract](contract.md) covers what a contract says.
- **The server** implements `NotesServer`, the trait that the group `Notes` gives, with one handler per route.
  `Api::router` makes an `axum::Router` of it, and `server::builder()` serves that router until Ctrl-C.
  [The server](server.md) covers services, layers and shutdown.
- **The client** is built by `client::builder()`. `api.notes()` has one method per route, which returns what the
  route returns. [The client](client.md) covers its middleware.
- **A handler's error**, `Error::not_found`, is answered 404 with an RFC 9457 problem. The client reads it back as a
  `quoll::Error` with that status. [Errors](errors.md) covers both sides.

## Run it

In one terminal:

```bash
cargo run -- serve
```

In another:

```bash
cargo run
```

It prints `note 1: Milk`. A call to a note that does not exist is an error with the status 404:

```bash
curl -i http://127.0.0.1:8080/api/notes/9
```

This writes the OpenAPI document. Any OpenAPI generator turns it into the client of a frontend:

```bash
cargo run -- openapi > openapi.json
```

The notes example has a page built on that document: [`examples/notes/web`](../../examples/notes/web). The example
uses orval, which writes the page's TanStack Query hooks from the document. [The TypeScript guide](typescript.md) says
how `cargo quoll` writes the document, runs the generator and runs the page.

## Crates by role

An application splits into crates by role. [The notes example](../../examples/notes) is split this way:

```text
notes/
  Cargo.toml              the workspace: its members, and the config of `cargo quoll`
  Cargo.lock
  api/      notes-api     the contract and its models: quoll, serde, schemars
  server/   notes-server  notes-api with the feature "server": tokio, axum
  cli/      notes-cli     notes-api with the feature "client": tokio, reqwest
  web/                    the page, whose TypeScript a generator writes from the document
```

The workspace's `Cargo.toml` names its members. `cargo build` and `cargo test` in its directory build them all:

```toml
[workspace]
members = ["api", "server", "cli"]
resolver = "3"
```

The contract crate turns on neither feature of `quoll`. It passes them on instead:

```toml
[features]
server = ["quoll/server"]
client = ["quoll/client"]

[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll" }
schemars = "1"
serde = { version = "1", features = ["derive"] }
```

The server depends on it with `features = ["server"]`, and the command line with `features = ["client"]`. So the
command line never links axum, and the server never links reqwest.

## Next

- [Authentication](authentication.md): routes that take a token, a key or a cookie.
- [Testing](testing.md): a server and its client in a test.
- [Another API](another-api.md): a contract of an API that quoll did not write, which the application calls.
- [OpenAPI](openapi.md): what the document holds.
- [TypeScript](typescript.md): the page, its hooks and its streams, from the document.
