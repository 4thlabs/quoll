# Testing

A test serves the router on a free port and calls it with the real client, over HTTP. Code written for the client
takes a fake as well. A snapshot of the OpenAPI document catches a change to the contract.

## A server and its client

`quoll::test::spawn(router)` serves a router on a free port of 127.0.0.1, and gives its URL. The server runs on the
test's tokio runtime: `#[tokio::test]` gives each test its own, which stops the server when the test returns.

```rust,test_harness
use quoll::rest::{self, Error, Result};
use quoll::{client, test};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<String> {
        match id {
            1 => Ok("Milk".into()),
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

`test::spawn` comes with the feature `server`, and the client with `client`: a test of the server turns on the
contract crate's `client` feature in its dev-dependencies.

```toml
[dependencies]
notes-api = { path = "../api", features = ["server"] }

[dev-dependencies]
notes-api = { path = "../api", features = ["client"] }
```

### With the server's layers

`server::Builder::into_router` gives the router with its layers, so that a test runs them as the server does. An
application that builds its server in a function serves the same in its tests:

```rust,test_harness
use std::time::Duration;

use quoll::rest::{self, Result};
use quoll::{client, server, test};

rest::api! {
    pub api Api = "/api";

    pub group Health {
        GET "/health" => health() -> String;
    }
}

struct App;

impl HealthServer for App {
    async fn health(&self) -> Result<String> {
        Ok("ok".into())
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

The trait of a group, `Notes`, is the one the client implements. Code written for `&impl Notes` takes the client, and
a fake in its tests, with no server:

```rust,test_harness
use std::collections::BTreeMap;
use std::sync::Mutex;

use quoll::rest::{self, Created, Error, Result};
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

rest::api! {
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
    async fn get(&self, id: i64) -> Result<Note> {
        let notes = self.notes.lock().expect("no test panics while holding the lock");

        notes
            .get(&id)
            .cloned()
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Note> {
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

A fake's methods have the client's signatures: no caller, and the `T` of a `Created<T>`. A route left out, or a
method of the wrong signature, does not compile.

## The OpenAPI snapshot

A test keeps a snapshot of the document beside it, and fails when the document changes, so that a change to the
contract is reviewed in the diff. It is written again when the test runs with `SNAPSHOT=overwrite`.

```rust,no_run,test_harness
use quoll::rest;

rest::api! {
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

In the notes example's directory, `examples/notes`, the snapshot of its contract is written again with:

```bash
SNAPSHOT=overwrite cargo test -p notes-api --test openapi
```

The test compares JSON values, so that the order of the keys does not count. It also catches a mistake the macro
cannot see: two schemes of different types and one `NAME`, from two blocks, make `Api::openapi()` panic.
