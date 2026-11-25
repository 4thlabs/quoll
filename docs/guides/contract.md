# The contract

A contract is a route table written in `quoll::api!`. The server, the client and the OpenAPI document all come from it.
This guide covers what a contract says. [The server](server.md) and [the client](client.md) cover how each side uses
it.

## A contract

```rust
use quoll::{Created, Events};
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

quoll::api! {
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

- `NotesServer`, the trait that the server implements, with the feature `server`;
- `ApiServices`, with one field per group, and `Api::router`, which gives the `axum::Router`, with `server`;
- `Notes`, the trait the client implements, `NotesClient`, `ApiClient` and `Api::client`, with the feature `client`;
- `Api::openapi()`, the [OpenAPI document](openapi.md), whatever the features.

The doc of `quoll::api!` lists them all. Nothing is written to disk: `cargo expand` shows the code, and `cargo
doc` shows the items with the contract's `///` comments.

## Methods and paths

- The methods are `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD` and `OPTIONS`.
- One string holds the path and the query: `"/notes/{id}"`, `"/notes?page={page}"`.
- Only the api has a prefix, `"/api"`, or `""` for none. The prefix has no parameter: a parameter that routes share is
  written in each path, `"/repos/{owner}/{repo}/issues"`.
- A route with no `->` returns nothing.
- The operation id of a route is its group in snake case and its function: `notes_get`. The OpenAPI document gives
  the operation that id, and tags it with its group.
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
use quoll::{Bytes, Form, Text};
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

quoll::api! {
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
             => write(id: i64, header("If-Match") version: Option<&str>, text: Text);
    }
}
```

### Borrowed arguments

A `&` on an argument's type makes the client borrow it. The server and the OpenAPI document receive the owned type.
A contract chooses argument by argument, and an argument with no `&` is taken by value:

| The contract writes                                  | The client takes | The server receives            |
| ---------------------------------------------------- | ---------------- | ------------------------------ |
| `owner: &str`                                        | `&str`           | `String`                       |
| `state: Option<&str>`                                | `Option<&str>`   | `Option<String>`               |
| `labels: &[&str]`                                    | `&[&str]`        | `Vec<String>`                  |
| `labels: &[String]`                                  | `&[String]`      | `Vec<String>`                  |
| `note: &NewNote`                                     | `&NewNote`       | `NewNote`                      |
| `login: &Form<Login>`, `text: &Text`, `file: &Bytes` | the same         | `Form<Login>`, `Text`, `Bytes` |
| `owner: String`, with no `&`                         | `String`         | `String`                       |

So a call writes `get("rust-lang", &repo)` with no `.into()`, and `create(&note)` sends a body that stays the
caller's, with no `Clone`.

- The macro reads the `&` alone: `&str` becomes `String`, `&[T]` becomes `Vec<T>`, and any other `&X` becomes `X`,
  nested ones included, as `Option<&str>`.
- `&[&str]` takes `&["a", "b"]`, and a `Vec<String>` as `&names.iter().map(String::as_str).collect::<Vec<_>>()`.
  `&[String]` takes `&names`, but not `&["a"]`.
- A call may borrow its arguments until it is awaited. A fake takes them as the client does, `owner: &str`, and reads
  them in its `async move`.
- With the server, `&mut X`, `&&X`, `&dyn Trait` and `&impl Trait` do not compile, nor a `&X` whose `X` has no size,
  as `&OsStr`: write `&str`, or take an `OsString` by value. A contract with a client alone takes them as before. A
  lifetime other than `'static` or `'_` does not compile.

### Path parameters

A `{name}` in the path is read from its text: an integer, a string, a UUID, or an enum of unit variants. It fills a
whole segment, as in `/notes/{id}`, never part of one.

### Query parameters

A `key={name}` in the query is a query parameter. An `Option` may be missing, and the client leaves out a `None`. A
`Vec` repeats its key, as in `?tag=a&tag=b`, and is empty when the key is missing.

A key is made of ASCII letters, digits and `-._~`. It may end in brackets that hold the same characters, or nothing.
Many APIs name their keys this way:

```rust
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Customer {
    pub id: String,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Article {
    pub title: String,
}

quoll::api! {
    pub api Api = "";

    pub group Shop {
        GET "/customers/{id}?expand[]={expand}"
            => customer(id: &str, expand: Vec<String>) -> Customer;
        GET "/articles?filter[name]={name}&page[size]={size}"
            => articles(name: Option<&str>, size: Option<u32>) -> Vec<Article>;
    }
}
```

The client escapes the brackets, like a browser's form: `?expand%5B%5D=sources&expand%5B%5D=invoice`. The server
reads them escaped or unescaped. The document names each parameter by its key, `expand[]`. A field of a query struct
takes such a key through its serde name: `#[serde(rename = "filter[name]")]`.

### Query structs

A route with many optional keys takes a query struct, `query filter: NoteFilter`. Its fields are the keys, with the
names that serde gives them. A query struct is the whole query, so the route's string has no query beside it. A `Vec`
field takes `#[serde(default)]`, so that a missing key reads as an empty list.

The struct needs no `Default`, but its callers like one. They fill the keys they need and leave the rest:
`NoteFilter { contains: Some("milk".into()), ..Default::default() }`.

### Joined lists

Some APIs read a list as one value, such as `labels=bug,docs`. A contract says so with `#[join(sep)]` on an argument
of the route. The argument's type stays a `Vec`:

```rust
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum State {
    Open,
    Closed,
}

quoll::api! {
    pub api Api = "";

    pub group Issues {
        GET "/repos/{owner}/{repo}/issues?labels={labels}&state={state}"
            => list(owner: &str, repo: &str, #[join(",")] labels: Vec<String>, state: Option<State>) -> Vec<String>;
    }
}
```

- `#[join(",")]` writes `labels=bug,docs`, `#[join(" ")]` writes `labels=bug%20docs`, and `#[join("|")]` writes
  `labels=bug|docs`. These are the three styles that OpenAPI describes. There is no other separator.
- The values are read from text, like a path parameter. An empty list leaves the key out. A missing key is an empty
  list, and so is `labels=`.
- The server decodes the value before it splits it. So it also reads `labels=bug%2Cdocs`, which a browser's
  `URLSearchParams` writes. A value therefore cannot hold the separator, and the client refuses one before sending.
- Only an argument is joined: a `Vec` field of a query struct repeats its key.

### Headers

`header("If-Match") version: Option<String>` reads a header from its text, like a path parameter. An `Option` is
`None` when the header is missing. With any other type, a missing header is answered 400. An argument does not compile
for a header that quoll or HTTP writes itself: for example `Content-Type`, `Host`, or `Authorization` on a route that
takes a scheme.

### The body

The one argument left is the body. Only `POST`, `PUT`, `PATCH` and `DELETE` take one. It is the JSON of its type, or
one of these:

- `Form<T>`, `application/x-www-form-urlencoded`;
- `Text`, `text/plain` in UTF-8;
- `Bytes`, bytes of the content type they carry: `application/octet-stream` unless they say another.

The server answers 415 to a body without the content type it reads, and 400 to one that does not read as its type.

The doc of the crate `quoll` gives the full rules of each form, such as an empty query value, a header given twice,
or a path parameter whose type is a struct.

## Fixed headers

Some APIs ask every request for headers of their own, such as a version or a media type. A contract says them with
`#[header("name" = value)]`, on the api, a group or a route:

```rust
use quoll::{Bytes, Text};
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct Repository {
    pub full_name: String,
}

/// The version of the REST API the contract follows.
pub const VERSION: &str = "2022-11-28";

quoll::api! {
    #[server(none)]
    #[header("Accept" = "application/vnd.github+json")]
    #[header("X-GitHub-Api-Version" = VERSION)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: &str, repo: &str) -> Repository;

        #[header("Accept" = "application/vnd.github.raw+json")]
        GET "/repos/{owner}/{repo}/readme" => readme(owner: &str, repo: &str) -> Bytes;

        // Leaves two headers of the api: a `Text` is read from `text/plain`.
        #[header("Accept" = none)]
        #[header("X-GitHub-Api-Version" = none)]
        GET "/zen" => zen() -> Text;
    }

    // Leaves every header of the api.
    #[header(none)]
    pub group Status {
        GET "/status" => status() -> String;
    }
}
```

- A value is a string or the name of a `&str` constant, checked at compile time. A value that changes from call to
  call is an argument: `header("Stripe-Account") account: Option<String>`. A value that comes from a deployment, a
  signature or a tenant goes through `client::on_request`.
- A group or a route takes its parent's headers, and the one nearest to the call wins. The order is: an argument for
  the same header, given by the caller, then the route, the group and the api. quoll's own `Accept` comes last. So
  under an api that fixes an `Accept`, a route whose result is `Text`, `Events` or `JsonLines` takes
  `#[header("Accept" = none)]`, like `/zen` above. Otherwise its answer comes in the api's media type, which is not its
  result.
- `#[header("X" = none)]` leaves out one header that the group or the route would take, and `#[header(none)]` leaves
  out all of them. The headers written beside it are its own. A `none` never removes what quoll writes:
  `#[header("Accept" = none)]` gives the route quoll's `Accept` back. In the api's block, a `none` does not compile on
  a header that the parent does not fix. In any block, a route's `none` does not compile on a header that its group
  already leaves out.
- A `Content-Type` keeps the media type of the body, and adds a parameter: `application/json; charset=utf-8` on a
  JSON body. A request with no body sends no `Content-Type`.
- A group of another block takes the headers of the api that lists it, and any api may list it. So a `none` in the
  group or in its routes leaves out the header where the api fixes it, and does nothing elsewhere.
- The server does not check them. The OpenAPI document describes each one as a header parameter with its value, which
  a caller need not send.

## Fixed fields of the body

Some APIs tell two answers of one method and path apart by a field of the body, such as Anthropic's
`"stream": true`. A contract says it with `#[body("name" = value)]`, on each route:

```rust
use quoll::Events;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MessageRequest {
    pub model: String,
    pub max_tokens: u32,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Message {
    pub id: String,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct StreamEvent {
    pub r#type: String,
}

quoll::api! {
    #[server(none)]
    pub api Anthropic = "/v1";

    pub group Messages {
        #[body("stream" = false)]
        POST "/messages" => create(request: MessageRequest) -> Message;

        #[body("stream" = true)]
        POST "/messages" => stream(request: MessageRequest) -> Events<StreamEvent>;
    }
}
```

- A route takes one `#[body(...)]` per field. Its value is a bool, a number or a string. The client adds each field
  at the top of the body: its JSON, or a `Form<T>`, when it serializes as a struct or a map. If the body has its own
  field of that name, the fixed value replaces it. Any other body is refused before it is sent, and a `Text` or
  `Bytes` body does not compile.
- Two routes share a method and a path when a field that both fix has two values of one kind. The server first reads
  the body's fields, which pick the route. Then it reads the handler's type from the same bytes, and that type ignores
  the fields. A type with `#[serde(deny_unknown_fields)]` refuses them: a 400 whose message names the field. A body
  that holds the fields of no route is answered 400. A content type that no route reads is answered 415.
- A route alone on its method and path checks its fields too, after it authenticates. A body that does not read as
  those fields gets a 400 of its own. Two routes that share a method and path read the body first, within its limit,
  to pick one, and then authenticate. So a body that matches no route, or that does not read, gets a 400 that names
  neither a route nor a field.
- The OpenAPI document describes a route's fields as `const` properties of its body. It does not describe two routes of
  one method and path yet. They compile only in an api with no document, which a contract of another API usually
  is, with `#[server(none)]`.

## Results

| The route returns | The server answers                                               | The client returns |
| ----------------- | ---------------------------------------------------------------- | ------------------ |
| nothing           | 204, no body                                                     | `()`               |
| `T`               | 200, the JSON of `T`                                             | `T`                |
| `Option<T>`       | 200, the JSON of the option, or 204                              | `Option<T>`        |
| `Created<T>`      | 201, the JSON of `T`, a `Location`, or 204 when `T` may be empty | `T`                |
| `Accepted<T>`     | 202, the JSON of `T`, or 204 when `T` may be empty               | `T`                |
| `Text`            | 200, `text/plain; charset=utf-8`, or 204                         | `Text`             |
| `Bytes`           | 200, their content type, or 204                                  | `Bytes`            |
| `Events<T>`       | 200, server-sent events, or 204                                  | a stream of `T`    |
| `JsonLines<T>`    | 200, one JSON value per line, or 204                             | a stream of `T`    |
| `ByteStream`      | 200, bytes as they come, or 204                                  | a stream of bytes  |
| `TcpTunnel`       | no server: the other api answers 101, the connection switched    | the connection     |

A handler may answer a 204 or a 205, with `Status::NO_CONTENT` or `Status::RESET_CONTENT`. It may do so only for a
value that may be empty:

- no result, an `Option<T>`, a `Text`, `Bytes` or a stream;
- a `Created<T>` or an `Accepted<T>` whose JSON `T` is a `()` or an `Option<T>`: `-> Created<Option<Note>>` and
  `-> Accepted<()>`, but not the `Note` of `-> Created<Note>`.

The client reads it as `()`, `None`, an empty text or bytes, or a stream that ends at once. The document lists it.
`NoContent::try_from(status)` gives a `NoContent` from the status of another API's answer.

A handler returns each of them in a `Response`, with the headers it sends. For a `Created<T>` or an `Accepted<T>`, the
response holds the `T` alone, and the contract gives the status:

```rust
use http::header::LOCATION;
use quoll::{Bytes, Response, Text};

let created = Response::new(7).header(LOCATION, "/api/notes/7");
let accepted = Response::new("export-1");
let readme = Text::from("# Notes");
let logo = Bytes::new(vec![0x89, b'P', b'N', b'G']).with_content_type("image/png");

assert_eq!(created.headers()[LOCATION], "/api/notes/7");
assert_eq!(accepted.body(), &"export-1");
assert_eq!(&*readme, "# Notes");
assert_eq!(logo.content_type(), "image/png");
```

A contract names these types of quoll:

- `Created` and `Accepted`;
- `Form`, `Text` and `Bytes`;
- `Events`, `JsonLines` and `ByteStream`;
- `TcpTunnel`, in a contract with `#[server(none)]` (see [A tunnel](#a-tunnel)).

They are imported like any other type, `use quoll::Created;`, or named by their path, `quoll::Bytes`.

## Streams

A route streams its result with one of three types:

- `Events<T>`: server-sent events, the JSON of a `T` in each;
- `JsonLines<T>`: JSON Lines, the JSON of a `T` on each line;
- `ByteStream`: bytes of one content type, such as the output of a process.

Each is one type on both sides. The server builds it from a `Stream` and sends its items as they come. The client gets
the same type: a `Stream` of `Result<T>`, or of `Result<Vec<u8>>` for a `ByteStream`.

```rust
use futures::{StreamExt, stream};
use quoll::{ByteStream, Events, Response, Result, client, test};

quoll::api! {
    pub api Api = "/api";

    pub group Live {
        GET "/notes/{id}/events" => events(id: i64) -> Events<String>;
        GET "/logs"              => logs() -> ByteStream;
    }
}

struct App;

impl LiveServer for App {
    async fn events(&self, id: i64) -> Result<Response<Events<String>>> {
        let changes = vec![format!("note {id} edited"), format!("note {id} removed")];

        Ok(Response::new(Events::new(stream::iter(changes))))
    }

    async fn logs(&self) -> Result<Response<ByteStream>> {
        let lines = vec![b"started\n".to_vec(), b"ready\n".to_vec()];

        Ok(Response::new(
            ByteStream::new(stream::iter(lines)).with_content_type("text/plain; charset=utf-8"),
        ))
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

- The server asks its stream for an item only once the connection has taken the last one. So a client that reads
  slowly slows it down.
- Dropping the client's stream closes its connection. The client does not call the route again when a stream ends.
- `try_new` builds a stream from results. An error after the 200 ends an `Events` stream with a last event, `event:
  problem`, which the client reads as that error. JSON Lines and bytes have no way to say an error: the server cuts
  the answer short, and the client reads an error.
- On the client, an item's error of the kind `ErrorKind::Decode`, an item that does not read, lets the stream go on.
  Any other kind ends it. [Errors in a stream](errors.md#errors-in-a-stream) shows it.
- A server told to stop ends its `Events` streams cleanly: [Shutdown](server.md#shutdown) says what becomes of each
  stream.
- A `ByteStream` goes out with no `Content-Length` (chunked encoding on HTTP/1). See [A length](#a-length) for one
  that has a length.
- `client::timeout` and `client::retry` treat a stream's answer differently:
  [Retries, deadlines and redirects](client.md#retries-deadlines-and-redirects) says how. `server::compression` leaves
  streams out.

The docs of `quoll::Events`, `JsonLines` and `ByteStream` say what each sends and reads on the wire.

### A length

A `ByteStream` whose size is known, such as a file, says it with `with_length`. The server sends it as
`Content-Length` rather than in chunked encoding. A browser then shows the size of the download and its progress, and
a client can tell a cut answer from a whole one.

```rust
use futures::stream;
use quoll::ByteStream;

let archive = vec![b"PK\x03\x04".to_vec()];
let download = ByteStream::new(stream::iter(archive))
    .with_content_type("application/zip")
    .with_length(4);

assert_eq!(download.length(), Some(4));
```

- A stream that gives another number of bytes is never answered as if it were whole. The server holds the chunk that
  brings the bytes to the length until the stream ends. A stream that gives more bytes, ends too early or fails is
  logged and cut before its last bytes, and the client reads an error.
- A length of 0 sends an empty body, and the stream is dropped unread.
- `length()` gives the bytes still to come: the length, less each chunk read. On the client it starts from the
  `Content-Length` of the answer. It is `None` for an answer in chunked encoding, or a compressed one that the client
  decodes.
- A handler that returns a client's `ByteStream` sends its length, or what is left of it when the handler has read
  some chunks already. A stream read past its length is answered with a 500, logged.
- A `HEAD` of the route answers its `Content-Length`, with no body.

## Groups across modules

A large contract splits into `quoll::group!` blocks, each with one group, in a module of its own. The api lists them
by their paths, after its prefix.

```rust
mod notes {
    quoll::group! {
        /// The signed-in user's notes.
        #[auth(Bearer | crate::Key)]
        pub group Notes {
            GET "/notes/{id}" => get(id: i64) -> String;
        }
    }
}

mod admin {
    quoll::group! {
        #[auth(crate::Key)]
        pub group Admin {
            GET "/admin/stats" => stats() -> u64;
        }
    }
}

quoll::api! {
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

- A `quoll::group!` holds one group, written the same way as in an api. Its items (`NotesServer`, `Notes` and
  `NotesClient`) are in its module.
- The api serves, calls and describes the routes of its groups under its own prefix. The groups of the list come
  first, then the ones its block writes.
- A group named alone, as in `{ Notes }`, is one in the api's module, or one that a glob import brings:
  `use notes::*;`.
- Schemes are declared only in the api. A `quoll::group!` names one by its path, `#[auth(crate::Key)]`, or through an
  import.
- A group takes the fixed headers of the api that lists it. Any api may list it. So a `none` in the group leaves out
  a header where that api fixes it, and does nothing elsewhere.
- The api checks its groups against each other at compile time. These do not compile, and the error points at the
  group in the list: one method and path twice, one operation id twice, or a group less visible than the api.
- Two apis may list one group: each serves it under its own prefix.

Two routes of one method and path compile when their [fixed fields](#fixed-fields-of-the-body) tell them apart, in an
api with no document, whatever groups they are in.

## One side

A contract has a server side and a client side, each written when `quoll` has its feature, `server` or `client`.
Cargo builds `quoll` once per binary, with every feature its dependents ask for. So in an application that serves its
own api and calls another, every contract gets both sides. `#[server(none)]` or `#[client(none)]` on the api says
which side a contract does not have, whatever the features:

```rust
mod github {
    use serde::Deserialize;

    /// A repository, which the client only reads.
    #[derive(Debug, Deserialize)]
    pub struct Repository {
        pub full_name: String,
    }

    quoll::api! {
        /// GitHub's REST API, which the application only calls.
        #[server(none)]
        pub api GitHub = "";

        pub group Repos {
            GET "/repos/{owner}/{repo}" => get(owner: &str, repo: &str) -> Repository;
        }
    }
}

mod hooks {
    use schemars::JsonSchema;
    use serde::Deserialize;

    /// An event another service sends, which the server only reads.
    #[derive(Debug, Deserialize, JsonSchema)]
    pub struct Event {
        pub kind: String,
    }

    quoll::api! {
        /// The webhooks of another service, which the application only serves.
        #[client(none)]
        pub api Hooks = "/hooks";

        pub group Events {
            POST "/events" => receive(event: Event);
        }
    }
}
```

- `#[server(none)]` is for an API that is only called, such as a third-party one. The contract has no server trait, no
  `ApiServices` and no `Api::router`. It has no OpenAPI document either, since the document describes what is served.
  Its types need only what the client reads and writes: `Deserialize` on what it reads, `Serialize` on what it sends.
  Two paths of one shape with their parameters named otherwise, `/repos/{owner}` and `/repos/{user}`, compile in it:
  only a router takes them for one.
- `#[client(none)]` is for an API that is only served, such as a webhook receiver. The contract has no client trait, no
  client and no `Api::client`. It keeps its OpenAPI document, unless it also says `#[openapi(none)]`. It takes no
  `#[error(E)]`, which only a client reads.
- The contract writes no item of the missing side, whatever the features: `impl NotesServer for App` on a contract
  with `#[server(none)]` is rustc's own error, `cannot find trait NotesServer in this scope`, and `Api::client` on one
  with `#[client(none)]` is `no associated function named client`. Those names stay free for the application.
- A contract with neither has both sides. The groups of the api's block have the api's sides.
- A `quoll::group!` says it on its group. An api lists only groups that have each side the api has. A group may have a
  side that the api does not: that api leaves it out, and another api that lists the group may use it.
- These do not compile: both on one contract, which would have no side, and an `#[openapi(...)]` beside
  `#[server(none)]`, which already leaves out the document.

### A tunnel

A route `-> TcpTunnel` asks the other api to switch its connection to `tcp`, as the Docker Engine API does for `exec`
and `attach`. Its answer is a `101 Switching Protocols`, and then the connection carries the bytes both ways, raw.
quoll's server does not open a tunnel yet, so the route compiles only in a contract with `#[server(none)]`:

```rust,no_run
use quoll::{TcpTunnel, client};
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

#[derive(Debug, Serialize)]
#[serde(rename_all = "PascalCase")]
pub struct ExecStart {
    pub detach: bool,
    pub tty: bool,
}

#[derive(Debug, Deserialize)]
pub struct DockerError {
    pub message: String,
}

impl std::fmt::Display for DockerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}

quoll::api! {
    #[server(none)]
    #[error(DockerError)]
    pub api Docker = "/v1.47";

    pub group Exec {
        /// The command's input and output, on the connection.
        POST "/exec/{id}/start" => start(id: &str, start: ExecStart) -> TcpTunnel;
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let docker = client::builder()
        .http(|http| http.unix_socket("/var/run/docker.sock"))
        .build::<Docker>("http://localhost")?;

    let mut shell = docker
        .exec()
        .start("8f1c", ExecStart { detach: false, tty: true })
        .await?;

    shell.write_all(b"ls; exit\n").await?;

    let mut output = Vec::new();
    shell.read_to_end(&mut output).await?;

    println!("{}", String::from_utf8_lossy(&output));
    Ok(())
}
```

- The request always sends `Connection: Upgrade` and `Upgrade: tcp`. The client's middlewares run on it as on any
  call: its credentials, its `User-Agent`, its retries. It works over TCP and over a Unix socket.
- The call gives a `TcpTunnel` once the other api has answered `101` with `Upgrade: tcp`, the case ignored. It reads
  with tokio's `AsyncRead` and writes with its `AsyncWrite`, so `tokio::io::split` and `tokio::io::copy` take it.
  `content_type()` gives the content type of the 101: Docker sends `application/vnd.docker.raw-stream` with a TTY,
  and its frames, `application/vnd.docker.multiplexed-stream`, without one.
- `shutdown()` closes the half the client writes: the other end sees the end of its input, as `cat` does, and the
  other half stays open. Dropping the tunnel closes the connection.
- `.timeout(d)` and `client::timeout` bound the wait for the 101 alone, as for the head of a stream. The tunnel has no
  deadline, and after the 101 its failures are the `std::io::Error` of a read or a write.
- An error answer, a 404 of an unknown exec, is read as on any route: `another api answered 404: No such exec
  instance: 8f1c`. A 2xx in place of the 101, or a 101 to another protocol, is an error of the kind
  `ErrorKind::Decode`. A 101 whose connection cannot be taken, because a middleware of the application's made a
  response of its own, is `ErrorKind::Closed`. An answer in HTTP/2, which has no 101, is `ErrorKind::Invalid`: a
  client that negotiates HTTP/2 over TLS is built with `.http(|http| http.http1_only())`.
- `.response()` gives the whole 101, its status and its headers, and the tunnel as its body.
- A fake returns `TcpTunnel::new(io)`, on one half of a `tokio::io::duplex`, with `with_content_type(..)` when the
  content type matters.
- The route has no server trait and no document, as every route of its contract. `TcpTunnel` is no body, no
  parameter, and no `T` of a `Created<T>` or an `Accepted<T>`.

## Mistakes

A mistake in a contract is a compile error on the token at fault, which says what to write instead. Here, no argument
has the name of `{id}`:

```rust,compile_fail
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(note: i64) -> String;
    }
}
```

The [spec](../spec.md#what-does-not-compile) lists every mistake that does not compile.
