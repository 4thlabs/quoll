# The contract

A contract is a route table written in `rest::api!`. The server, the client and the OpenAPI document all come from it.
This guide covers what a contract says. [The server](server.md) and [the client](client.md) cover how each side uses
it.

## A contract

```rust
use quoll::rest::{self, Created, Events};
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

/// Each key given narrows the search.
#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    pub contains: Option<String>,
    pub page: Option<u32>,
}

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    /// The signed-in user's notes.
    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes"             => search(query filter: NoteFilter) -> Vec<Note>;
        /// A note, by its id.
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        GET    "/notes/{id}/events" => events(id: i64) -> Events<Note>;
    }
}
```

Each line is a route: the method, the path below the api's prefix, then the Rust function it becomes. The block holds
the api once, the groups of its routes, and the [schemes](authentication.md#schemes) it declares.

The contract becomes items in the module that writes it:

- `NotesServer`, the trait the server implements, with the feature `server`;
- `ApiServices`, a field per group, and `Api::router`, which gives the `axum::Router`, with `server`;
- `Notes`, the trait the client implements, `NotesClient`, `ApiClient` and `Api::client`, with the feature `client`;
- `Api::openapi()`, the [OpenAPI document](openapi.md), whatever the features.

The doc of `quoll::rest::api!` lists them all. Nothing is written to disk: `cargo expand` shows the code, and `cargo
doc` shows the items with the contract's `///` comments.

## Methods and paths

- The methods are `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD` and `OPTIONS`.
- One string holds the path and the query: `"/notes/{id}"`, `"/notes?page={page}"`.
- Only the api has a prefix, `"/api"`, or `""` for none. The prefix has no parameter: a parameter that routes share is
  written in each path, `"/repos/{owner}/{repo}/issues"`.
- A route with no `->` returns nothing.
- The operation id of a route is its group in snake case and its function: `notes_get`. The OpenAPI document names the
  operation so, and tags it with its group.
- The `///` comments of the api, its groups and its routes become the docs of the generated items, and the
  descriptions of the document.

## Parameters, by name

Each argument is matched to the route's string by its name, not by its position. An argument is one of these:

| Written as                                 | It is                 |
| ------------------------------------------ | --------------------- |
| `{id}` in the path, and `id: i64`          | a path parameter      |
| `key={name}` in the query, and `name: T`   | a query parameter     |
| `query filter: NoteFilter`                 | a query struct        |
| `header("If-Match") version: Option<...>`  | a header              |
| any other argument                         | the body              |

```rust
use quoll::rest::{self, Bytes, Form, Text};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    pub contains: Option<String>,
    #[serde(default)]
    pub tags: Vec<String>,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Login {
    pub user: String,
    pub password: String,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{id}"               => get(id: i64) -> String;
        GET  "/notes?page={page}&tag={tags}"
             => list(page: Option<u32>, tags: Vec<String>) -> Vec<String>;
        GET  "/search"                   => search(query filter: NoteFilter) -> Vec<String>;
        POST "/notes"                    => create(title: String) -> i64;
        POST "/token"                    => token(login: Form<Login>) -> String;
        PUT  "/notes/{id}/file"          => upload(id: i64, file: Bytes);
        PUT  "/notes/{id}/text"
             => write(id: i64, header("If-Match") version: Option<String>, text: Text);
    }
}
```

### Path parameters

A `{name}` of the path is read from its text: an integer, a string, a UUID, or an enum of unit variants. It fills a
whole segment, `/notes/{id}`, never part of one.

### Query parameters

A `key={name}` of the query is a query parameter. An `Option` may be missing, and the client leaves out a `None`. A
`Vec` repeats its key, `?tag=a&tag=b`, and is empty when the key is missing.

### Query structs

A route with many optional keys takes a query struct, `query filter: NoteFilter`: its fields are the keys, as serde
names them. A query struct is the whole query, so the route's string has no query beside it. A `Vec` field takes
`#[serde(default)]`, so that a missing key reads as an empty list.

The struct needs no `Default`, but its callers like one: they fill the keys they need and leave the rest,
`NoteFilter { contains: Some("milk".into()), ..Default::default() }`.

### Headers

`header("If-Match") version: Option<String>` reads a header, from its text as a path parameter is. An `Option` is
`None` when the header is missing; any other type answers 400 then. A header quoll or HTTP writes itself, as
`Content-Type`, `Host` or `Authorization` on a route that takes a scheme, does not compile.

### The body

The one argument left is the body, on `POST`, `PUT`, `PATCH` and `DELETE` only. It is the JSON of its type, or one of:

- `Form<T>`, `application/x-www-form-urlencoded`;
- `Text`, `text/plain` in UTF-8;
- `Bytes`, bytes of the content type they carry, `application/octet-stream` unless they say another.

The server answers 415 to a body without the content type it reads, and 400 to one that does not read as its type.

The doc of `quoll::rest` gives the rules of each form in full: an empty query value, a header given twice, a path
parameter whose type is a struct.

## Results

| The route returns | The server answers                   | The client returns |
| ----------------- | ------------------------------------ | ------------------ |
| nothing           | 204, no body                         | `()`               |
| `T`               | 200, the JSON of `T`                 | `T`                |
| `Created<T>`      | 201, the JSON of `T`, a `Location`   | `T`                |
| `Accepted<T>`     | 202, the JSON of `T`                 | `T`                |
| `Text`            | 200, `text/plain; charset=utf-8`     | `Text`             |
| `Bytes`           | 200, their content type              | `Bytes`            |
| `Events<T>`       | 200, server-sent events              | a stream of `T`    |
| `JsonLines<T>`    | 200, one JSON value per line         | a stream of `T`    |
| `ByteStream`      | 200, bytes as they come              | a stream of bytes  |

A handler builds each of them:

```rust
use quoll::rest::{Accepted, Bytes, Created, Text};

let created = Created::at("/api/notes/7", 7);
let without_location = Created::new(7);
let accepted = Accepted::new("export-1");
let readme = Text::from("# Notes");
let logo = Bytes::new(vec![0x89, b'P', b'N', b'G']).with_content_type("image/png");

assert_eq!(created.location(), Some("/api/notes/7"));
assert_eq!(without_location.location(), None);
assert_eq!(accepted.value(), &"export-1");
assert_eq!(&*readme, "# Notes");
assert_eq!(logo.content_type(), "image/png");
```

The types a contract names, `Created`, `Accepted`, `Form`, `Text`, `Bytes`, `Events`, `JsonLines` and `ByteStream`,
are imported as any type is, `use quoll::rest::{self, Created};`, or named by their path, `rest::Bytes`.

## Streams

A route streams its result with one of three types:

- `Events<T>`: server-sent events, the JSON of a `T` in each;
- `JsonLines<T>`: JSON Lines, the JSON of a `T` on each line;
- `ByteStream`: bytes of one content type, as the output of a process.

Each is one type on both sides. The server builds it from a `Stream` and sends its items as they come. The client gets
the same type, a `Stream` of `Result<T>`, or of `Result<Vec<u8>>` for a `ByteStream`.

```rust
use futures::{StreamExt, stream};
use quoll::rest::{self, ByteStream, Events, Result};
use quoll::{client, test};

rest::api! {
    pub api Api = "/api";

    pub group Live {
        GET "/notes/{id}/events" => events(id: i64) -> Events<String>;
        GET "/logs"              => logs() -> ByteStream;
    }
}

struct App;

impl LiveServer for App {
    async fn events(&self, id: i64) -> Result<Events<String>> {
        let changes = vec![format!("note {id} edited"), format!("note {id} removed")];

        Ok(Events::new(stream::iter(changes)))
    }

    async fn logs(&self) -> Result<ByteStream> {
        let lines = vec![b"started\n".to_vec(), b"ready\n".to_vec()];

        Ok(ByteStream::new(stream::iter(lines)).with_content_type("text/plain; charset=utf-8"))
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let url = test::spawn(Api::router(ApiServices { live: App })).await;
    let api = client::builder().build::<Api>(&url)?;

    let mut events = api.live().events(3).await?;

    while let Some(event) = events.next().await {
        println!("{}", event?);
    }

    let mut logs = api.live().logs().await?;

    assert_eq!(logs.content_type(), "text/plain; charset=utf-8");

    while let Some(chunk) = logs.next().await {
        print!("{}", String::from_utf8_lossy(&chunk?));
    }

    Ok(())
}
```

How a stream behaves:

- The server asks its stream for an item only once the connection has taken the last one: a client that reads slowly
  slows it down.
- Dropping the client's stream closes its connection. The client does not call the route again when a stream ends.
- `try_new` builds a stream from results. An error after the 200 ends an `Events` stream with a last event, `event:
  problem`, which the client reads as that error. JSON Lines and bytes have no way to say an error: the server cuts
  the answer short, and the client reads an error.
- `client::timeout` bounds the head of a stream's answer alone, `client::retry` tries that head again, and
  `server::compression` leaves streams out.

The docs of `quoll::rest::Events`, `JsonLines` and `ByteStream` say what each sends and reads on the wire.

## Groups across modules

A large contract splits into `rest::group!` blocks, one group in each, in a module of its own. The api lists them by
their paths after its prefix.

```rust
mod notes {
    use quoll::rest;

    rest::group! {
        /// The signed-in user's notes.
        #[auth(Bearer | crate::Key)]
        pub group Notes {
            GET "/notes/{id}" => get(id: i64) -> String;
        }
    }
}

mod admin {
    use quoll::rest;

    rest::group! {
        #[auth(crate::Key)]
        pub group Admin {
            GET "/admin/stats" => stats() -> u64;
        }
    }
}

use quoll::rest;

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api" { notes::Notes, admin::Admin };

    /// The key of an operator's scripts.
    pub scheme Key = header "X-Api-Key";

    pub group Health {
        GET "/health" => health() -> String;
    }
}

fn main() {
    println!("{}", Api::openapi().to_json_pretty());
}
```

- A `rest::group!` holds one group, written as in an api. Its items, `NotesServer`, `Notes` and `NotesClient`, are in
  its module.
- The api serves, calls and describes the routes of its groups under its own prefix. The groups of the list come
  first, then the ones its block writes.
- A group named alone, `{ Notes }`, is one in the api's module, or brought by a glob import, `use notes::*;`.
- Schemes are declared in the api alone. A `rest::group!` names one by its path, `#[auth(crate::Key)]`, or through an
  import.
- The api checks its groups against each other at compile time: one method and path twice, one operation id twice, a
  group less visible than the api, do not compile, and the error points at the group in the list.
- Two apis may list one group: each serves it under its own prefix.

## Mistakes

A mistake in a contract is a compile error on the token at fault, which says what to write instead. Here `{id}` has no
argument of its name:

```rust,compile_fail
use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(note: i64) -> String;
    }
}
```

The [spec](../spec.md#what-does-not-compile) lists every mistake that does not compile.
