# Testing

A test serves the router on a free port and calls it with the real client, over HTTP. Code written for the client
takes a fake as well. A snapshot of the OpenAPI document catches a change to the contract.

## A server and its client

`quoll::test::spawn(router)` serves a router on a free port of 127.0.0.1, and gives its URL. The server runs on the
test's tokio runtime. `#[tokio::test]` gives each test its own runtime, which stops the server when the test returns.

```rust,test_harness
use quoll::{Error, Response, Result, client, test};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<String>> {
        match id {
            1 => Ok(Response::new("Milk".into())),
            _ => Err(Error::not_found("no such note")),
        }
    }
}

fn router() -> axum::Router {
    Api::router(ApiServices { notes: App })
}

#[tokio::test]
async fn a_note_reads_by_its_id() -> anyhow::Result<()> {
    let url = test::spawn(router()).await;
    let api = client::builder().build::<Api>(&url)?;

    assert_eq!(api.notes().get(1).await?, "Milk");
    Ok(())
}

#[tokio::test]
async fn a_missing_note_is_a_404() -> anyhow::Result<()> {
    let url = test::spawn(router()).await;
    let api = Api::client(&url)?;

    let error = api.notes().get(2).await.unwrap_err();

    assert_eq!(error.status(), Some(http::StatusCode::NOT_FOUND));
    Ok(())
}
```

`test::spawn` comes with the feature `server`, and the client with `client`. So a test of the server turns on the
`client` feature of the contract crate, in its dev-dependencies.

```toml
[dependencies]
notes-api = { path = "../api", features = ["server"] }

[dev-dependencies]
notes-api = { path = "../api", features = ["client"] }
```

### With the server's layers

`server::Builder::into_router` gives the router with its layers, so that a test runs them the same way the server
does. An application that builds its server in a function serves the same server in its tests:

```rust,test_harness
use std::time::Duration;

use quoll::{Response, Result, client, server, test};

quoll::api! {
    pub api Api = "/api";

    pub group Health {
        GET "/health" => health() -> String;
    }
}

struct App;

impl HealthServer for App {
    async fn health(&self) -> Result<Response<String>> {
        Ok(Response::new("ok".into()))
    }
}

/// The server's layers, which `main` serves and the tests too.
fn server() -> server::Builder {
    server::builder()
        .with(server::request_id())
        .with(server::timeout(Duration::from_secs(10)))
}

#[tokio::test]
async fn the_server_answers_with_its_layers() -> anyhow::Result<()> {
    let router = server().into_router(Api::router(ApiServices { health: App }));
    let url = test::spawn(router).await;
    let api = client::builder().build::<Api>(&url)?;

    assert_eq!(api.health().health().await?, "ok");
    Ok(())
}
```

## Fakes

The trait of a group, `Notes`, is the one that the client implements. Code written for `&impl Notes` takes the
client, and a fake in its tests, with no server:

```rust,test_harness
use std::collections::BTreeMap;
use std::sync::Mutex;

use quoll::{Arg, Call, Created, Error, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
}

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{id}" => get(id: i64) -> Note;
        POST "/notes"      => create(note: NewNote) -> Created<Note>;
    }
}

/// Code of the application: a copy of a note, through any client of the group.
async fn copy(notes: &impl Notes, id: i64) -> Result<Note> {
    let note = notes.get(id).await?;

    notes
        .create(NewNote {
            title: format!("{} (copy)", note.title),
        })
        .await
}

/// Notes kept in memory, in the place of a server.
#[derive(Default)]
struct Fake {
    notes: Mutex<BTreeMap<i64, Note>>,
}

impl Notes for Fake {
    fn get(&self, id: impl Arg<i64>) -> impl Call<Note> {
        let id = id.into_value();

        async move {
            let notes = self.notes.lock().expect("no test panics while holding the lock");

            notes
                .get(&id)
                .cloned()
                .ok_or_else(|| Error::not_found("no such note"))
        }
    }

    fn create(&self, note: impl Arg<NewNote>) -> impl Call<Note> {
        let note = note.into_value();

        async move {
            let mut notes = self.notes.lock().expect("no test panics while holding the lock");
            let id = notes.len() as i64 + 1;
            let note = Note {
                id,
                title: note.title,
            };

            notes.insert(id, note.clone());
            Ok(note)
        }
    }
}

#[tokio::test]
async fn a_copy_has_the_title_of_its_note() -> Result<()> {
    let fake = Fake::default();
    let milk = fake
        .create(NewNote {
            title: "Milk".into(),
        })
        .await?;

    let copied = copy(&fake, milk.id).await?;

    assert_eq!(copied.title, "Milk (copy)");
    Ok(())
}
```

A fake's methods have the client's signatures: no caller, each argument an `impl Arg<T>` of its type in the contract,
the `T` of a `Created<T>`, and an `impl Call<T>`. `into_value()` gives the argument's value, before the block or
inside it, and an `async move` block is an `impl Call<T>`. A route left out, or a method with the wrong signature,
does not compile. Two messages of rustc do not name `impl Arg`, and both mean the same signature to write,
`fn get(&self, id: impl Arg<i64>)` for `get(id: i64)`:

- ``method `get` has 0 type parameters but its trait declaration has 1 type parameter`` (E0049): the fake takes
  `id: i64`, the contract's type alone.
- `impl has stricter requirements than trait` (E0276): the fake takes `id: impl Arg<String>`, an `Arg` of another
  type than the contract's.

Some apis answer their errors in a shape of their own, with `#[error(E)]`. A fake of such an api returns an
`impl Call<T, E>`. It builds the error of an answer with `quoll::Error::answered(StatusCode::NOT_FOUND, e)`:
[Another API's errors](errors.md#another-apis-errors) shows one.

## The OpenAPI snapshot

A test keeps a snapshot of the document beside it, and fails when the document changes. So a change to the contract
is reviewed in the diff. The test writes the snapshot again when it runs with `SNAPSHOT=overwrite`.

```rust,no_run,test_harness
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

const SNAPSHOT: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/openapi.snapshot.json");

#[test]
fn the_document_is_the_snapshot() {
    let written = Api::openapi().to_json_pretty() + "\n";

    if std::env::var_os("SNAPSHOT").is_some_and(|value| value == "overwrite") {
        std::fs::write(SNAPSHOT, &written).expect("the snapshot is written");
        return;
    }

    let snapshot = std::fs::read_to_string(SNAPSHOT)
        .expect("the snapshot is there: write it with SNAPSHOT=overwrite");
    let expected: serde_json::Value = serde_json::from_str(&snapshot).expect("the snapshot is JSON");
    let actual: serde_json::Value = serde_json::from_str(&written).expect("the document is JSON");

    assert!(
        actual == expected,
        "the document is not the snapshot: run the test with SNAPSHOT=overwrite and review its diff"
    );
}
```

In the directory of the notes example, `examples/notes`, this writes the snapshot of its contract again:

```bash
SNAPSHOT=overwrite cargo test -p notes-api --test openapi
```

The test compares JSON values, so the order of the keys does not count. It also catches a mistake that the macro
cannot see: two schemes of different types with one `NAME`, from two blocks, make `Api::openapi()` panic.
