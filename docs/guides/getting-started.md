# Getting started

This guide builds a first contract, its server and its client, in one crate, and runs them.

## The crate

quoll is used from git until it is published. A new crate takes it with both features, and the crates a contract
needs: serde for the bodies, schemars for the OpenAPI document, and tokio to run.

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

use quoll::rest::{self, Created, Error, Result};
use quoll::{client, server};
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
    async fn get(&self, id: i64) -> Result<Note> {
        let notes = self.notes.lock().expect("no handler panics while holding the lock");

        notes
            .iter()
            .find(|note| note.id == id)
            .cloned()
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Created<Note>> {
        let mut notes = self.notes.lock().expect("no handler panics while holding the lock");
        let note = Note {
            id: notes.len() as i64 + 1,
            title: note.title,
        };

        notes.push(note.clone());
        Ok(Created::at(format!("/api/notes/{}", note.id), note))
    }
}

async fn serve() -> std::io::Result<()> {
    let router = Api::router(ApiServices {
        notes: App::default(),
    });

    server::builder().serve("127.0.0.1:8080", router).await
}

async fn call() -> rest::Result<()> {
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
async fn main() -> Result<(), Box<dyn std::error::Error>> {
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
- **The contract**, `rest::api!`, is a route table: `GET "/notes/{id}"` becomes a function `get(id: i64)`, which
  returns a `Note`. [The contract](contract.md) covers what a contract says.
- **The server** implements `NotesServer`, the trait the group `Notes` gives, with a handler per route.
  `Api::router` makes an `axum::Router` of it, which `server::builder()` serves until Ctrl-C.
  [The server](server.md) covers services, layers and shutdown.
- **The client** is built by `client::builder()`: `api.notes()` has a method per route, which returns what the route
  returns. [The client](client.md) covers its middleware.
- **A handler's error**, `Error::not_found`, is answered 404 with an RFC 9457 problem, which the client reads back as
  a `rest::Error` with that status. [Errors](errors.md) covers both sides.

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

And the OpenAPI document, which orval turns into TypeScript:

```bash
cargo run -- openapi > openapi.json
```

The notes example has a page on it, [`examples/notes/web`](../../examples/notes/web), whose hooks orval writes from
that document: [the web side](openapi.md#the-web-side) runs it.

## Crates by role

An application splits into crates by role, as [the notes example](../../examples/notes) does:

```text
notes/
  Cargo.toml              the workspace: its members, and the config of `cargo quoll`
  Cargo.lock
  api/      notes-api     the contract and its models: quoll, serde, schemars
  server/   notes-server  notes-api with the feature "server": tokio, axum
  cli/      notes-cli     notes-api with the feature "client": tokio, reqwest
  web/                    the page, whose TypeScript orval writes from the document
```

The workspace's `Cargo.toml` names its members, and `cargo build` and `cargo test` in its directory build them all:

```toml
[workspace]
members = ["api", "server", "cli"]
resolver = "3"
```

The contract crate turns on neither feature of `quoll`, and passes them on:

```toml
[features]
server = ["quoll/server"]
client = ["quoll/client"]

[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll" }
schemars = "1"
serde = { version = "1", features = ["derive"] }
```

The server depends on it with `features = ["server"]`, the command line with `features = ["client"]`: the command
line never links axum, nor the server reqwest.

## Next

- [Authentication](authentication.md): routes that take a token, a key or a cookie.
- [Testing](testing.md): a server and its client in a test.
- [OpenAPI](openapi.md): what the document holds.
