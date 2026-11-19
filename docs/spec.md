
# The REST contract

> [!NOTE]
> **Validated** by the maintainer on 2026-11-19.

A REST contract written once in Rust, from which a macro generates a server on axum, a client on reqwest and an OpenAPI document; the OpenAPI document feeds TypeScript generators (TanStack Query, SWR, zod...). No UI. It takes the REST spec of the C++ Quoll, now [quoll-http](https://github.com/4thlabs/quoll-http), as its model.

Decided by the maintainer on 2026-11-18: the contract is one function-like macro, `rest::api! { ... }`; the contract says which authentication a route takes, `#[auth(Bearer)]`, and the server's handler receives the authenticated caller as a typed argument; authentication is generic, through schemes; no generated Rust is written to disk; the client is built on reqwest-middleware and is native only, with no wasm32 for now. On 2026-11-19: the crate is named `quoll`; errors stay one `rest::Error`, typed errors per route come later; the items of a stream are typed in TypeScript through an extension and a `subscribe` helper; an `i64` is a TypeScript `number`; pagination by header comes later.

## Stack and crates

- **tokio** for the runtime, **axum** (on hyper and tower) for the server, **reqwest** with **reqwest-middleware** for the client. Native only: the browser goes through the TypeScript.
- **serde** and **serde_json** for bodies, **serde_html_form** for forms and queries (it repeats a key for a `Vec`), **schemars** for the JSON Schema of each type, which OpenAPI 3.1 uses as is.
- **syn** and **quote** in the macro.
- The server hands back an `axum::Router`, the client takes a `reqwest_middleware::ClientWithMiddleware`: tower layers and reqwest middleware stay the user's, with no wrapper in between.

An application splits into crates by role:

```text
notes/
  api/      notes-api     the contract and its models: quoll, serde, schemars; no tokio, axum or reqwest
  server/   notes-server  notes-api with feature "server": tokio, axum
  cli/      notes-cli     notes-api with feature "client": tokio, reqwest
  web/      TypeScript    vite, orval, TanStack Query
```

The crate is `quoll`, used from git until it is published; the contract is its module `quoll::rest`, which the examples bring in with `use quoll::rest`. It has two features, `server` and `client`. What the macro generates for each side compiles only with its feature: the command line never links axum, nor the server reqwest. A contract crate passes them on, `notes-api = { path = "../api", features = ["server"] }`.

## The contract

An api is a block read as a route table: the method, the path, then the Rust function it becomes.

```rust
rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Form<Login>) -> TokenResponse;
    }

    /// The signed-in user's notes.
    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes?contains={contains}&page={page}"
               => list(contains: Option<String>, page: Option<u32>) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}"        => remove(id: i64);
    }
}
```

### Methods and paths

- `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`.
- One string holds the path below the api's prefix and the query: `"/notes/{id}"`, `"/notes?contains={contains}&page={page}"`.
- Only the api has a prefix, with no parameter. A parameter that routes share goes in each path: `"/repos/{owner}/{repo}/issues"`.
- The operation id is the group and the function, `notes_get`; the OpenAPI tag is the group.
- `///` comments become the OpenAPI summary and description, and the rustdoc of what is generated.
- A large api splits into `rest::group! { pub group Notes { ... } }` blocks, one per module, and an api that lists them: `pub api Api = "/api" { Auth, Notes };`.

### Parameters, by name

An argument is matched to the string by its name, not its position. Each one is:

- **a path parameter**: its name is a `{name}` of the path, read from text through serde as axum's `Path` reads it (integers, strings, UUIDs, unit enums);
- **a query parameter**: its name is a `key={name}` of the query; an `Option` may be missing, a `Vec` repeats the key;
- **a query struct**: `query filter: NoteFilter`, whose fields are the keys as serde names them; not with a query in the string;
- **a header**: `header("Last-Event-ID") last: Option<String>`;
- **the body**: the one argument left, on `POST`, `PUT`, `PATCH` or `DELETE` only; JSON, or `Form<T>`, `Text`, `Bytes`.

### Results

| The route returns | Status | On the wire |
|---|---|---|
| nothing (no `->`) | 204 | no body |
| `T` | 200 | JSON |
| `Created<T>` | 201 | JSON, with an optional `Location` |
| `Accepted<T>` | 202 | JSON |
| `Text`, `Bytes` | 200 | `text/plain`, `application/octet-stream` |
| `Events<T>` | 200 | server-sent events, one JSON value each |
| `JsonLines<T>` | 200 | one JSON value per line |

`Events<T>` and `JsonLines<T>` are one type on both sides: the server builds one from a `Stream` of values, the client gets a `Stream` of `Result<T>`.

## Authentication

The contract says which authentication a group or a route takes; the server says how a credential becomes a caller, and which type that caller is. The contract names no type of it: the caller lives on the server.

```rust
#[auth(Bearer)]
pub group Notes {
    GET "/notes/{id}" => get(id: i64) -> Note;
}
```

```rust
// server
impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        self.sessions.user(&token).ok_or_else(|| Error::unauthorized("unknown or expired token"))
    }
}

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64) -> Result<Note> { ... }
}
```

| On a group or a route | The handler receives | Without a valid credential |
|---|---|---|
| `#[auth(Bearer)]` | `&Caller` | 401 |
| `#[auth(Bearer \| Key)]` | `&Caller`, from whichever credential the request carries; both schemes give the same type | 401 |
| `#[auth(Bearer, optional)]` | `Option<&Caller>` | none without a credential, 401 with a wrong one |
| `#[auth(Bearer, scopes("repo"))]` | `&Caller` | 401, or 403 when the caller lacks a scope |
| `#[auth(none)]` on a route | nothing | the route leaves its group's authentication |

- The caller comes after `&self`, before the route's own arguments, in the server's trait only. A route's attribute replaces its group's.
- A service whose group takes `Bearer` implements `Authenticate<Bearer>`: the server's trait requires it, so a service that cannot authenticate does not compile. Its `authenticate` runs before the handler; an error answers 401.
- With scopes, the caller implements `rest::Scoped`, and the router answers 403 before the handler. OpenAPI lists each route's scopes.
- The client's trait has no caller: its middleware sends the credential.

```rust
pub trait Authenticate<S: Scheme>: Send + Sync + 'static {
    type Caller: Send + Sync + 'static;

    async fn authenticate(&self, credential: S::Credential) -> Result<Self::Caller>;
}

pub trait Scoped {
    fn has_scope(&self, scope: &str) -> bool;
}
```

### Schemes

A scheme is a type implementing `rest::Scheme`: how the server reads a credential, and how OpenAPI describes it.

```rust
pub trait Scheme {
    type Credential;

    fn read(request: &http::request::Parts) -> Result<Option<Self::Credential>>;
    fn openapi() -> SecurityScheme;
}
```

- Given by the crate: `Bearer` and `Basic`. Declared in the contract, with a name: `scheme Key = header "X-Api-Key";`, `scheme Session = cookie "session";`, `scheme Token = query "token";`, `scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";` (a bearer token on the wire, described as OpenID Connect).
- Any other is a type of the application implementing `Scheme`, as an HMAC signature, named in `#[auth(...)]` as the others are.

## What the macro generates

Nothing is written to disk: the macro runs as rustc compiles the contract, and its code is compiled in place. `cargo expand` or rust-analyzer's "Expand macro recursively" show it; `cargo doc` shows what is generated, with the contract's comments.

| Item | For | What it is |
|---|---|---|
| `trait NotesServer` | server | one method per route, with the caller its authentication gives |
| `trait Notes` | client | one method per route, implemented by the client and by fakes in tests |
| `struct ApiServices` | server | a field per group: its service |
| `Api::router` | server | an `axum::Router` from an `ApiServices` |
| `Api::client`, `Api::client_with` | client | `api.notes()` is a `NotesClient`, implementing `Notes` |
| `Api::openapi` | both | the OpenAPI 3.1 document |
| `struct Key`, ... | both | each scheme the contract declares |

The generated code stays thin and calls the crate, compiled once. A client method, for `GET "/notes/{id}" => get(id: i64) -> Note`:

```rust
impl Notes for NotesClient {
    async fn get(&self, id: i64) -> rest::Result<Note> {
        let path = format!("/api/notes/{}", rest::segment(&id));

        self.client.get(path, &ROUTE_NOTES_GET).send().await?.json().await
    }
}
```

Each method becomes a function returning `impl Future + Send`, which axum needs; an implementation still writes `async fn`.

## The server

- A service implements the server trait of its group; one type may implement several.
- A service of a group with authentication also implements `Authenticate` for its schemes (see Authentication).
- `ApiServices` has one field per group: a missing one does not compile.
- A service is `Send + Sync + 'static`; the router holds it in an `Arc`. Its state is its own fields: there is no axum `State` to thread through.

## The client

- `Api::client(url)` makes a client with no middleware; `Api::client_with(http, url)` takes a `ClientWithMiddleware`.
- `api.notes().get(3)`: a method per route, arguments in the contract's order. Rust has no named arguments: a route with many optional filters takes a query struct with `Default`.
- Each request carries a `rest::Route` in its extensions: the operation id, the group, the method, the path as written and the schemes it accepts. Middleware reads it.

## Middleware

One mechanism a side: reqwest-middleware on the client, tower layers on the server. The crate gives the usual ones, built on the existing crates where there is one.

### Client, `rest::client::`

| Helper | What it does |
|---|---|
| `on_request(f)` | a closure that changes each request before it leaves, given its `rest::Route`: what an interceptor does elsewhere |
| `bearer(token)` | `Authorization: Bearer`, on the routes whose schemes hold `Bearer` only |
| `bearer_from(source)` | asks a token source before each call, and again once after a 401, for tokens that expire |
| `basic(user, password)` | `Authorization: Basic`, on the routes that take it |
| `key::<Key>(value)` | a key in the header, cookie or query its scheme declares |
| `user_agent(name)` | `User-Agent` |
| `request_id()` | a new `X-Request-Id` per call |
| `retry(times)` | backoff on a failure with no response, 502, 503 or 504; `GET`, `HEAD`, `PUT` and `DELETE` only (reqwest-retry) |
| `timeout(duration)` | a deadline per call |
| `trace()` | a tracing span per call, named by the operation id (reqwest-tracing) |

A header computed from the request is a closure given to `on_request`; the helpers of authentication are written so:

```rust
let sign = rest::client::on_request(move |request: &mut Request, route: &rest::Route| {
    let body = request.body().and_then(|body| body.as_bytes()).unwrap_or_default();
    let signature = key.sign(request.method(), request.url().path(), body);

    request.headers_mut().insert("X-Signature", signature);
});
```

A full `Middleware`, which wraps the call with `next.run(request)`, is for what also sees the response: a retry, a token refreshed after a 401, a span around the call.

### Server, `rest::server::`

| Helper | What it does |
|---|---|
| `request_id()` | keeps the caller's `X-Request-Id` or makes one, and sends it back |
| `trace()` | a tracing span per request, with the operation id and the request id (tower-http) |
| `timeout(duration)` | a handler that takes longer answers 503 |
| `body_limit(bytes)` | a larger body answers 413 |
| `cors(origins)` | CORS for the given origins (tower-http) |
| `compression()` | gzip, brotli and zstd responses (tower-http) |

They answer RFC 9457 problems, as handlers do, where tower-http's answer plain text. Any other tower layer applies as is, as a rate limit from tower_governor.

## Errors

- `rest::Result<T>` is `Result<T, rest::Error>`, one error for the server and the client: its `status()`, the RFC 9457 problem when there is one, the raw body, and `body_as::<T>()` for a third-party API's own error shape.
- The server answers a handler's error with its status and a problem in JSON: `Error::not_found(detail)`, `Error::bad_request(detail)`, `Error::unauthorized(detail)`, `Error::new(status, detail)`, and `Error::internal(err)` for a 500 that logs `err` without sending it.
- An error a client received from another API answers 502 when a handler returns it, so that the other API's status is not passed on as ours.
- On the client, a failure with no response (connection, timeout) is an `Error` without a status.
- Every route has this one error, and its TypeScript the problem shape. Typed errors per route, `Result<Note, NotFound>` with `#[derive(rest::Problem)] #[problem(status = 404)]`, come later: adding them breaks no contract.

## OpenAPI and TypeScript

- `Api::openapi()` returns the OpenAPI 3.1 document: paths, parameters, bodies, results, schemas from schemars, security schemes, the problem shape of errors. A binary of the contract crate prints it: the schemas are known only once the types are compiled, which a macro does not see.
- orval writes the TypeScript from it: TanStack Query here; `vue-query`, `svelte-query`, `swr`, `zod` or a plain `fetch` client from the same document.
- The credential goes through orval's mutator, one function of the application; the browser sends cookies itself.
- A stream's items are typed through an extension, as OpenAPI 3.1 cannot type them: the response's media type (`text/event-stream`, `application/jsonl`) carries `x-item`, a `$ref` to the item's schema, which orval writes as a type like any other. `subscribe<T>(url, onItem, init)`, from `vite-plugin-rest/client`, reads the stream with `fetch`, so that it sends headers, calls `onItem` with each item parsed, and returns the function that stops it. The extension goes once the generators read OpenAPI 3.2's item schemas.
- An `i64` is a `number`, exact up to 2^53: ids and counts stay below it. A value that may go beyond it is declared as a `String` in its Rust type.
- Neither the document nor the TypeScript is committed: the Vite plugin writes them in development and in a build. They are the one generated code on disk, as `tsc` and the editor need files.

### The Vite plugin

- **`vite dev`**: builds and starts the server, proxies `/api` to it, writes `openapi.json`, runs orval through its JavaScript API.
- **A change to the contract crate**: writes `openapi.json` again, runs orval, rebuilds and restarts the server. The contract crate holds no axum, so its binary builds in seconds. A file whose content did not change is not rewritten, so that Vite reloads nothing.
- **A change to the server crate**: rebuilds and restarts the server only.
- **A cargo error** shows in Vite's overlay and its terminal; the last server that built keeps running.
- **`vite build`**: writes the TypeScript before bundling, and fails if cargo fails.

## A complete example

Notes kept per user: a token for a login, notes to list, read, write and watch, and statistics for operators holding a key. The web page and the command line use bearer tokens, the operators' scripts a key.

`api/Cargo.toml`

```toml
[package]
name = "notes-api"
edition = "2024"

[features]
server = ["quoll/server"]
client = ["quoll/client"]

[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll" }
schemars = "1"
serde = { version = "1", features = ["derive"] }
```

`api/src/lib.rs`

```rust
use quoll::rest::{self, Created, Events, Form};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
    pub body: String,
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
    pub body: String,
}

#[derive(Default, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    pub contains: Option<String>,
    pub tag: Option<String>,
    pub page: Option<u32>,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum NoteEvent {
    Edited(Note),
    Removed { id: i64 },
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Login {
    pub user: String,
    pub password: String,
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct TokenResponse {
    pub access_token: String,
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Stats {
    pub users: u64,
    pub notes: u64,
}

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    /// The key of an operator's scripts.
    scheme Key = header "X-Api-Key";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Form<Login>) -> TokenResponse;
    }

    /// The signed-in user's notes.
    #[auth(Bearer)]
    pub group Notes {
        /// Notes matching the filter, a page at a time.
        GET    "/notes"             => search(query filter: NoteFilter) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        /// The changes of a note, as they happen.
        GET    "/notes/{id}/events" => events(id: i64) -> Events<NoteEvent>;
    }

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> Stats;
    }
}
```

`api/src/bin/openapi.rs`

```rust
fn main() {
    println!("{}", notes_api::Api::openapi().to_json_pretty());
}
```

`server/src/lib.rs`

```rust
use notes_api::*;
use quoll::rest::{self, Authenticate, Bearer, Created, Error, Events, Form, Result};

mod store;

pub use store::{Sessions, Store};

/// Who a request comes from, once its token is checked. Never sent: the server alone builds it.
#[derive(Clone)]
pub struct User {
    pub id: i64,
    pub name: String,
}

/// An operator, known by its key.
#[derive(Clone)]
pub struct Operator {
    pub name: String,
}

#[derive(Clone)]
pub struct App {
    pub store: Store,
    pub sessions: Sessions,
}

impl AuthServer for App {
    async fn token(&self, login: Form<Login>) -> Result<TokenResponse> {
        let user = self
            .store
            .check_password(&login.user, &login.password)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::unauthorized("wrong user or password"))?;

        Ok(TokenResponse { access_token: self.sessions.open(user) })
    }
}

impl NotesServer for App {
    async fn search(&self, user: &User, filter: NoteFilter) -> Result<Vec<Note>> {
        self.store.notes(user.id, &filter).await.map_err(Error::internal)
    }

    async fn get(&self, user: &User, id: i64) -> Result<Note> {
        self.store
            .note(user.id, id)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, user: &User, note: NewNote) -> Result<Created<Note>> {
        let note = self.store.add(user.id, note).await.map_err(Error::internal)?;

        Ok(Created::at(format!("/api/notes/{}", note.id), note))
    }

    async fn update(&self, user: &User, id: i64, note: NewNote) -> Result<Note> {
        self.store
            .replace(user.id, id, note)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn remove(&self, user: &User, id: i64) -> Result<()> {
        self.store.remove(user.id, id).await.map_err(Error::internal)
    }

    async fn events(&self, user: &User, id: i64) -> Result<Events<NoteEvent>> {
        let changes = self.store.watch(user.id, id).await.map_err(Error::internal)?;

        Ok(Events::new(changes))
    }
}

impl AdminServer for App {
    async fn stats(&self, _operator: &Operator) -> Result<Stats> {
        self.store.stats().await.map_err(Error::internal)
    }
}

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        self.sessions.user(&token).ok_or_else(|| Error::unauthorized("unknown or expired token"))
    }
}

impl Authenticate<Key> for App {
    type Caller = Operator;

    async fn authenticate(&self, key: String) -> Result<Operator> {
        self.store.operator(&key).await.map_err(Error::internal)?.ok_or_else(|| Error::unauthorized("unknown key"))
    }
}

pub fn router(app: App) -> axum::Router {
    Api::router(ApiServices {
        auth: app.clone(),
        notes: app.clone(),
        admin: app,
    })
    .layer(rest::server::request_id())
    .layer(rest::server::trace())
    .layer(rest::server::timeout(Duration::from_secs(10)))
    .layer(rest::server::body_limit(1 << 20))
}
```

`server/src/main.rs`

```rust
#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let app = notes_server::App {
        store: notes_server::Store::open("notes.db").await?,
        sessions: notes_server::Sessions::default(),
    };
    let listener = tokio::net::TcpListener::bind("0.0.0.0:8080").await?;

    axum::serve(listener, notes_server::router(app)).await?;
    Ok(())
}
```

`server/tests/notes.rs`

```rust
use notes_api::{Api, NewNote, Notes};
use notes_server::User;
use quoll::rest;
use reqwest_middleware::ClientBuilder;

#[tokio::test]
async fn a_created_note_reads_back() -> anyhow::Result<()> {
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User { id: 1, name: "ana".into() });
    let url = rest::test::spawn(notes_server::router(app)).await;
    let http = ClientBuilder::new(reqwest::Client::new()).with(rest::client::bearer(token)).build();
    let api = Api::client_with(http, &url)?;

    let created = api.notes().create(NewNote { title: "Milk".into(), body: String::new() }).await?;
    let read = api.notes().get(created.id).await?;

    assert_eq!(read.title, "Milk");
    Ok(())
}

#[tokio::test]
async fn a_call_without_token_is_refused() -> anyhow::Result<()> {
    let url = rest::test::spawn(notes_server::router(notes_server::App::in_memory())).await;
    let api = Api::client(&url)?;

    let error = api.notes().get(1).await.unwrap_err();

    assert_eq!(error.status(), Some(reqwest::StatusCode::UNAUTHORIZED));
    Ok(())
}
```

`cli/src/main.rs`

```rust
use futures::StreamExt;
use notes_api::{Api, Login, NewNote, NoteFilter, Notes};
use quoll::rest::{self, Form};
use reqwest::StatusCode;
use reqwest_middleware::ClientBuilder;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let url = std::env::var("NOTES_URL")?;
    let login = Login { user: std::env::var("NOTES_USER")?, password: std::env::var("NOTES_PASSWORD")? };
    let token = Api::client(&url)?.auth().token(Form(login)).await?;

    let http = ClientBuilder::new(reqwest::Client::new())
        .with(rest::client::user_agent("notes-cli/0.1"))
        .with(rest::client::request_id())
        .with(rest::client::retry(3))
        .with(rest::client::bearer(token.access_token))
        .build();
    let api = Api::client_with(http, &url)?;

    let created = api.notes().create(NewNote { title: "Milk".into(), body: String::new() }).await?;
    let filter = NoteFilter { contains: Some("milk".into()), ..Default::default() };

    for note in api.notes().search(filter).await? {
        println!("{} {}", note.id, note.title);
    }

    match api.notes().get(999).await {
        Err(error) if error.status() == Some(StatusCode::NOT_FOUND) => println!("no note 999"),
        found => println!("{:?}", found?),
    }

    let mut events = api.notes().events(created.id).await?;

    while let Some(event) = events.next().await {
        println!("{:?}", event?);
    }

    Ok(())
}
```

`web/vite.config.ts`

```typescript
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";
import rest from "vite-plugin-rest";

export default defineConfig({
  plugins: [
    react(),
    rest({
      workspace: "..",
      contract: { package: "notes-api", bin: "openapi" },
      server: { package: "notes-server", url: "http://localhost:8080", proxy: "/api" },
      orval: "./orval.config.ts",
    }),
  ],
});
```

`web/orval.config.ts`

```typescript
import { defineConfig } from "orval";

export default defineConfig({
  notes: {
    input: "./openapi.json",
    output: {
      target: "./src/api/generated.ts",
      client: "react-query",
      httpClient: "fetch",
      mode: "tags-split",
      override: { mutator: { path: "./src/api/fetch.ts", name: "apiFetch" } },
    },
  },
});
```

`web/src/api/fetch.ts`

```typescript
import { subscribe } from "vite-plugin-rest/client";

let token: string | undefined;

export function signIn(accessToken: string) {
  token = accessToken;
}

export async function apiFetch<T>(url: string, init: RequestInit): Promise<T> {
  const headers = new Headers(init.headers);

  if (token) {
    headers.set("Authorization", `Bearer ${token}`);
  }

  const response = await fetch(url, { ...init, headers });

  if (!response.ok) {
    throw await response.json(); // the RFC 9457 problem
  }

  return (response.status === 204 ? undefined : await response.json()) as T;
}

export function apiSubscribe<T>(url: string, onItem: (item: T) => void): () => void {
  const headers: Record<string, string> = token ? { Authorization: `Bearer ${token}` } : {};

  return subscribe<T>(url, onItem, { headers });
}
```

`web/src/NoteView.tsx`

```typescript
import { useEffect } from "react";

import { apiSubscribe } from "./api/fetch";
import { type NoteEvent, useNotesGet, useNotesUpdate } from "./api/generated";

export function NoteView({ id }: { id: number }) {
  const { data: note, refetch } = useNotesGet(id);
  const update = useNotesUpdate();

  // Another tab or the command line changed the note: read it again.
  useEffect(() => apiSubscribe<NoteEvent>(`/api/notes/${id}/events`, () => refetch()), [id, refetch]);

  if (!note) {
    return <p>Loading</p>;
  }

  const save = (body: string) => update.mutate({ id, data: { title: note.title, body } });

  return <NoteEditor note={note} onSave={save} />;
}
```

## What does not compile

- A `{name}` with no argument of that name; an argument that is neither in the string, a header, the query struct nor the one body.
- Two bodies; a body on `GET`, `HEAD` or `OPTIONS`.
- A path parameter whose type is not read from text, such as a `Vec` or a struct.
- A query struct and a query in the string together.
- One method and path twice in an api.
- A route left out of a service, a handler of the wrong signature: Rust's own errors on the trait.
- A group without a service; a service of a group with authentication that does not implement `Authenticate` for its schemes; two schemes of one `#[auth(A | B)]` giving callers of two types; scopes on a caller that does not implement `Scoped`.
- A scheme in `#[auth(...)]` that is neither given by the crate, declared in the contract nor a type implementing `Scheme`.

Each message the macro writes points at the token at fault and says what it expected, and has a test (trybuild).

## Version A, the other spelling

Attributes on a trait. Not chosen, but kept possible: both spellings are read into one model of the api, from which one generator writes the code, so A can be added later, or groups of both sit in one api. Neither spelling says anything the other cannot.

```rust
#[rest::group(auth = Bearer)]
pub trait Notes {
    #[get("/notes/{id}")]
    async fn get(&self, id: i64) -> Result<Note>;

    #[post("/notes")]
    async fn create(&self, note: NewNote) -> Result<Created<Note>>;
}

#[rest::api(prefix = "/api", groups(Notes))]
pub struct Api;
```

rustfmt formats it and rust-analyzer reads it as any Rust; it repeats `&self`, `async` and `Result` on every route. B reads as a table of routes, and the compiler checks both the same way.

## Later

Decided, not in the first version. Neither changes a contract already written.

1. **Typed errors per route**, as the Errors section says.
2. **Pagination by header.** GitHub pages with a `Link` header (`rel="next"`, `rel="last"`), which a result cannot say yet: a result `Paged<T>` carrying it, and on the client `.pages()`, a stream that follows the links.
