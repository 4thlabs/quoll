<div align="center">
  <h1><img src="docs/assets/quoll.svg" alt="" height="112" align="absmiddle" /> Quoll</h1>

  <p><strong>Contract-first REST for Rust: one contract, enforced by the compiler, from your server to your
  frontend.</strong><br />
  An axum server, a reqwest client and an OpenAPI 3.1 document from one macro. <code>cargo quoll</code> writes the
  document, runs your frontend's generator, and serves both while you work.</p>

  <p>
    <img src="https://img.shields.io/badge/Rust-2024-B7410E?logo=rust&logoColor=white" alt="Rust 2024" />
    <img src="https://img.shields.io/badge/OpenAPI-3.1-6BA539?logo=openapiinitiative&logoColor=white" alt="OpenAPI 3.1" />
    <img src="https://img.shields.io/badge/status-from%20git-orange" alt="Status: from git" />
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT" /></a>
  </p>

  <p>
    <a href="docs/guides/getting-started.md"><strong>Getting started</strong></a> ·
    <a href="docs/README.md"><strong>The guides</strong></a> ·
    <a href="docs/guides/typescript.md">TypeScript</a> ·
    <a href="examples/notes">The notes example</a> ·
    <a href="docs/spec.md">The spec</a> ·
    <a href="https://github.com/4thlabs/quoll/milestone/5">Later</a>
  </p>
</div>

## The contract

[The contract](docs/guides/contract.md) is a route table, written once in a macro: the method, the path, then the
Rust function it becomes. `Note` and `NewNote` are serde types, with a JSON Schema for the document.

<!-- doctest contract: models.rs -->
```rust
use quoll::Created;

quoll::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}
```

## A client

[The client](docs/guides/client.md) has one method per route. Each method takes the route's arguments and returns
its result. A call that fails returns a [`quoll::Error`](docs/guides/errors.md), which holds the status the server
answered:

<!-- doctest client: models.rs contract -->
```rust,no_run
use http::StatusCode;
use quoll::client;

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let api = client::builder()
        .with(client::bearer("ana-token"))
        .build::<Api>("http://127.0.0.1:8080")?;

    match api.notes().get(3).await {
        Ok(note) => println!("{}", note.title),
        Err(error) if error.status() == Some(StatusCode::NOT_FOUND) => println!("no note 3"),
        Err(error) => return Err(error),
    }

    Ok(())
}
```

## A server

[The server](docs/guides/server.md) implements one trait per group, with one handler per route, and serves them on
axum. A handler receives the caller: the `User` that `Authenticate<Bearer>` makes from the token.
[Authentication](docs/guides/authentication.md) shows how:

<!-- doctest server: models.rs contract server.rs -->
```rust,no_run
use http::header::LOCATION;
use quoll::{Error, Response, Result, server};

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64) -> Result<Response<Note>> {
        self.store
            .note(user.id, id)
            .map(Response::new)
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, user: &User, note: NewNote) -> Result<Response<Note>> {
        let note = self.store.add(user.id, note);
        let location = format!("/api/notes/{}", note.id);

        Ok(Response::new(note).header(LOCATION, location))
    }

    async fn remove(&self, user: &User, id: i64) -> Result<Response<()>> {
        self.store.remove(user.id, id);
        Ok(Response::new(()))
    }
}

#[tokio::main]
async fn main() -> std::io::Result<()> {
    let router = Api::router(ApiServices {
        notes: App::default(),
    });

    server::builder()
        .with(server::trace())
        .serve("0.0.0.0:8080", router)
        .await
}
```

## The document

[The OpenAPI 3.1 document](docs/guides/openapi.md) comes from the contract. It holds the schemas of the contract's
types and the contract's `///` comments. A frontend gets its client from the document:
[TypeScript](docs/guides/typescript.md) shows how. A binary of the contract's crate prints the document:

<!-- doctest document: models.rs contract -->
```rust
println!("{}", Api::openapi().to_json_pretty());
```

Any OpenAPI generator reads it: orval, openapi-typescript, hey-api, Kiota... The notes example uses orval, which
writes TanStack Query hooks. `cargo quoll gen` writes the document and runs the generator that the workspace names.
`cargo quoll dev` runs the server and the page's dev server side by side.

## Quick start

[Getting started](docs/guides/getting-started.md) goes through this crate step by step. quoll is used from git until
it is published. Create a crate with `cargo new notes`, then add quoll with both features to its `Cargo.toml`:

```toml
[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll", features = ["server", "client"] }
schemars = "1"
serde = { version = "1", features = ["derive"] }
tokio = { version = "1", features = ["macros", "rt-multi-thread"] }
```

`src/main.rs` holds a contract, its server and its client:

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

`cargo run -- serve` serves it on `127.0.0.1:8080`. In another terminal, `cargo run` calls it and prints
`note 1: Milk`. `cargo run -- openapi` prints its OpenAPI document.

[The guides](docs/README.md) cover each part.

## What the contract gives

| From the contract | What it is                                                                             |
| ----------------- | -------------------------------------------------------------------------------------- |
| `NotesServer`     | The trait the server implements: a method per route, the caller as a typed argument    |
| `Api::router`     | An `axum::Router` of the services in `ApiServices`, which `server::builder()` serves   |
| `Notes`           | The trait the client implements, and [a test's fake](docs/guides/testing.md#fakes) too |
| `ApiClient`       | The client that `client::builder().build::<Api>(url)` gives, with its middleware       |
| `Api::openapi`    | The OpenAPI 3.1 document, with the schemas of the types and the `///` comments         |

No Rust is generated on disk: the macro expands at compile time, and `cargo expand` shows what it writes.

## Examples

Each example is its own workspace, built and tested from its directory.

[`examples/notes`](examples/notes) is the spec's [complete example](docs/spec.md#a-complete-example): a contract,
its server, a command line, and a web page built on the hooks that orval writes. `cargo quoll dev` runs the server,
orval and Vite, on `http://localhost:5173`:

```bash
cargo install --git https://github.com/4thlabs/quoll quoll-cli
cd examples/notes
npm ci --prefix web
cargo quoll dev
```

[`examples/docker`](examples/docker) holds a client of the Docker Engine API, with no server, and a command line
that uses it. Its contract describes an API that quoll did not write. [Another API](docs/guides/another-api.md) shows
how to write such a contract.

```bash
cd examples/docker
cargo run -p docker-cli -- ps -a
```

## Agents

**Made for agents.** The contract is checked when it compiles: a route without its handler, a call with the wrong
arguments, or a client out of step with the server is a compile error, spanned on the token at fault and saying what
to write instead. An agent that gets it wrong finds out at once, and fixes it from the message.

The compiler checks the contract, not what a handler does: [a test](docs/guides/testing.md) does, a server and its
client in a few lines.

## Status

quoll is used from git. It has no version yet, and the first release is
[#21](https://github.com/4thlabs/quoll/issues/21). Everything [the spec](docs/spec.md) describes works, except its
section [Later](docs/spec.md#later). What works:

- the contract, the server and the client;
- the OpenAPI document;
- `cargo quoll gen`, `build` and `dev`;
- the notes page.

What comes next is in the milestone [Later](https://github.com/4thlabs/quoll/milestone/5).

The guides are in [`docs/`](docs/README.md). The API documentation is the crate's rustdoc: `cargo doc -p quoll
--all-features --open` opens it.

## License

MIT, see [`LICENSE.md`](LICENSE.md).
