<div align="center">
  <img src="docs/assets/quoll.svg" alt="" width="104" />

  <h1>Quoll</h1>

  <p><strong>A REST contract for Rust, written once: an axum server, a reqwest client and an OpenAPI document come
  from it.</strong><br />
  TypeScript generators such as orval turn the document into TanStack Query hooks.</p>

  <p>
    <img src="https://img.shields.io/badge/Rust-2024-B7410E?logo=rust&logoColor=white" alt="Rust 2024" />
    <img src="https://img.shields.io/badge/OpenAPI-3.1-6BA539?logo=openapiinitiative&logoColor=white" alt="OpenAPI 3.1" />
    <img src="https://img.shields.io/badge/status-design-lightgrey" alt="Status: design" />
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT" /></a>
  </p>

  <p>
    <a href="docs/README.md"><strong>The guides</strong></a> ·
    <a href="docs/spec.md"><strong>The spec</strong></a> ·
    <a href="docs/spec.md#a-complete-example">A complete example</a> ·
    <a href="docs/spec.md#later">Later</a>
  </p>
</div>

## One contract, both sides

An API is written once, in a macro. The server implements it, the client calls it, and a mistake on either side, a
route without its handler, a call with the wrong arguments, does not compile.

```rust,ignore
rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}
```

<table>
<tr>
<th>The server</th>
<th>The client</th>
</tr>
<tr>
<td>

```rust,ignore
impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        self.sessions
            .user(&token)
            .ok_or_else(|| Error::unauthorized("unknown or expired token"))
    }
}

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64) -> Result<Note> {
        self.store
            .note(user.id, id)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    // create, remove...
}

server::builder()
    .with(server::request_id())
    .with(server::trace())
    .serve("0.0.0.0:8080", Api::router(ApiServices { notes: app }))
    .await?;
```

</td>
<td>

```rust,ignore
let api = client::builder()
    .with(client::bearer(token))
    .with(client::retry(3))
    .build::<Api>("http://127.0.0.1:8080")?;

let note = api.notes().get(3).await?;

println!("{}", note.title);
```

</td>
</tr>
</table>

## Quick start

quoll is used from git until it is published. A new crate, `cargo new notes`, takes it with both features in its
`Cargo.toml`:

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

`cargo run -- serve` serves it, on `127.0.0.1:8080`. `cargo run`, in another terminal, calls it, and prints
`note 1: Milk`. `cargo run -- openapi` prints its OpenAPI document.

[Getting started](docs/guides/getting-started.md) goes through it, and [the guides](docs/README.md) cover each part.

## What the contract gives

| From the contract | What it is                                                                                      |
| ----------------- | ----------------------------------------------------------------------------------------------- |
| `NotesServer`     | The trait the server implements: a method per route, the caller as a typed argument             |
| `Api::router`     | An `axum::Router`, which `server::builder()` serves with request id, trace, CORS...             |
| `Notes`           | The trait the client implements, and test fakes too                                             |
| `ApiClient`       | The client that `client::builder().build::<Api>(url)` gives, with tokens, retries, redirects... |
| `Api::openapi`    | The OpenAPI 3.1 document, with the schemas of the types and the `///` comments                  |

No Rust is generated on disk: the macro expands at compile time, and `cargo expand` shows what it writes.

## Status

The contract is specified in [`docs/spec.md`](docs/spec.md), validated, and the crate implements it: its server, its
client and its OpenAPI document. It is used from git until it is published.

The guides are in [`docs/`](docs/README.md), and the API documentation is the crate's own, from `cargo doc -p quoll
--all-features --open`. Two examples are built and
tested with the workspace: [`examples/notes`](examples/notes), the spec's complete example, and
[`examples/docker`](examples/docker), a client alone of the Docker Engine API.

## License

MIT, see [`LICENSE.md`](LICENSE.md).
