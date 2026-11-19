
# The REST contract

> [!NOTE]
> **Validated** by the maintainer on 2026-11-19.

A REST contract written once in Rust, from which a macro generates a server on axum, a client on reqwest and an OpenAPI document; the OpenAPI document feeds TypeScript generators (TanStack Query, SWR, zod...). No UI. It takes the REST spec of the C++ Quoll, now [quoll-http](https://github.com/4thlabs/quoll-http), as its model.

Decided by the maintainer on 2026-11-18: the contract is one function-like macro, `rest::api! { ... }`; the contract says which authentication a route takes, `#[auth(Bearer)]`, and the server's handler receives the authenticated caller as a typed argument; authentication is generic, through schemes; no generated Rust is written to disk; the client is built on reqwest-middleware and is native only, with no wasm32 for now. On 2026-11-19: the crate is named `quoll`; errors stay one `rest::Error`, typed errors per route come later; the items of a stream are typed in TypeScript through an extension and a `subscribe` helper; an `i64` is a TypeScript `number`; pagination by header comes later; the error of `authenticate` answers its own status, not always 401; a path parameter of a struct type is refused when it is called, until a `rest::PathValue` derive refuses it at compile time; a header read twice, an argument in two places, a query struct that is not a struct and a header that quoll or HTTP writes do not compile; `Bytes` carries its content type; a failure after the 200 ends an `Events` stream with an `event: problem`, and the client reads every event that has data, whatever its name; a scheme gives its name and its challenge as `Scheme::NAME` and `Scheme::CHALLENGE`, its credential is `Send`, and `openapi` comes with the OpenAPI document; `Basic` reads a `BasicCredential` and challenges with the realm `api`; a declared scheme is written with its visibility, `pub scheme Key`; in `Bearer | Key` the first credential the request carries decides; a route needs all its scopes, and `optional` with `scopes` does not compile; `key::<K>` sends any declared scheme's credential, `openid`'s included, and the client's middleware tell a route's schemes by their type; a cookie scheme refuses a request from another origin, as Go's `CrossOriginProtection` does, the server's own origin alone trusted for now; an alternative takes up to four schemes; `Api::openapi()` returns a `rest::openapi::Document` and is written whatever the features, so that every type of a contract implements `JsonSchema`, unless the api says `#[openapi(none)]`; `Scheme::openapi` returns a `rest::openapi::SecurityScheme` and has no default; the document's `info` is the contract crate's name and version, a query struct is unfolded into its keys, `Bytes` are `application/octet-stream` as a body and `*/*` as a result, each route lists the errors quoll answers and a `default`, two types of one name take schemars' suffix, a type read and written otherwise has two schemas, operation ids are unique, two schemes of one `NAME` in an api do not compile, and the notes document is a snapshot of its tests.

## Stack and crates

- **tokio** for the runtime, **axum** (on hyper and tower) for the server, **reqwest** with **reqwest-middleware** for the client. Native only: the browser goes through the TypeScript.
- **serde** and **serde_json** for bodies, **serde_html_form** for forms and queries (it repeats a key for a `Vec`), **schemars** for the JSON Schema of each type, which OpenAPI 3.1 uses as is. Every type a contract names implements `JsonSchema`, whatever the features, as the document is written with neither.
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
- **the body**: the one argument left, on `POST`, `PUT`, `PATCH` or `DELETE` only; JSON, or `Form<T>`, `Text`, `Bytes`. `Bytes` carries its content type, any media type but the three a browser sends to another site without a preflight (`application/x-www-form-urlencoded`, `multipart/form-data`, `text/plain`), which answer 415, as a missing one does.

### Results

| The route returns | Status | On the wire |
|---|---|---|
| nothing (no `->`) | 204 | no body |
| `T` | 200 | JSON |
| `Created<T>` | 201 | JSON, with an optional `Location` |
| `Accepted<T>` | 202 | JSON |
| `Text` | 200 | `text/plain` |
| `Bytes` | 200 | its content type, `application/octet-stream` by default |
| `Events<T>` | 200 | server-sent events, one JSON value each |
| `JsonLines<T>` | 200 | one JSON value per line |

On the client, a `Created<T>` or `Accepted<T>` route returns the `T`. A `Bytes` result is read whatever its content type, which it keeps; with none, it is `application/octet-stream`. A `Text` result is read from `text/plain` in UTF-8 only.

`Events<T>` and `JsonLines<T>` are one type on both sides: the server builds one from a `Stream` of values, the client gets a `Stream` of `Result<T>`. A failure after the 200 ends an `Events` stream with an `event: problem`, whose data is the problem, which the client reads as that error; the client reads every event that has data, whatever its name.

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
- `#[auth(Bearer | Key)]` tries the schemes in the order written, up to four: the first whose credential the request carries decides, and a wrong one is refused with its error, the next not tried. A request may carry two credentials, a session cookie beside a token: neither is refused for the other. A 401 carries one `WWW-Authenticate` per scheme of the route, in that order.
- `optional` gives `None` for a request with no credential of any of its schemes; a credential that is malformed or that `authenticate` refuses is answered as on any route.
- A service whose group takes `Bearer` implements `Authenticate<Bearer>`: the server's trait requires it, so a service that cannot authenticate does not compile. Its `authenticate` runs before the handler, and its error answers its own status: 401 for an unknown or expired credential, 403 for a caller refused, 500 for a failure of the service, logged, so that a client does not drop a valid token when the store is down.
- With scopes, the caller implements `rest::Scoped`, and the router answers 403 before the handler, after `authenticate` and before the path, the query and the body are read, when the caller lacks one of them: a route needs every scope it names, `scopes("repo", "admin")` both. `optional` with `scopes` does not compile: a request with no credential has no scope to check. OpenAPI lists each route's scopes.
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

A scheme is a type implementing `rest::Scheme`: how the server reads a credential, its name, the challenge of a 401, and how OpenAPI describes it.

```rust
pub trait Scheme: 'static {
    type Credential: Send + 'static;

    const NAME: &'static str;
    const CHALLENGE: HeaderValue = HeaderValue::from_static(Self::NAME);

    fn read(request: &http::request::Parts) -> Result<Option<Self::Credential>>;
    fn openapi() -> SecurityScheme;
}
```

- `NAME` is what `rest::Route::schemes()` holds and what OpenAPI names the scheme; `CHALLENGE` is the `WWW-Authenticate` of a 401, a constant so that one that is not a header's value does not compile. The credential is `Send`, as `authenticate` holds it across an `await`. `openapi` describes the scheme in the document, with no default: `SecurityScheme::bearer()`, `basic()`, `http(name)`, `header(name)`, `cookie(name)`, `query(name)` or `openid(url)`, `.with_description(text)`. The crate's schemes and the declared ones give theirs, a declared one with its `///` as the description.
- `read` gives `None` for a request with none of its credential, leaving to another scheme an `Authorization` header of another scheme, and a 401 for one of its own that is not well formed. Its detail never shows the credential.
- Given by the crate: `Bearer`, whose credential is the token, a `String`; and `Basic`, whose credential is a `rest::BasicCredential { user, password }`, read from the base64 of `user:password` in UTF-8, and whose `Debug` hides the password. `Basic` challenges with `Basic realm="api", charset="UTF-8"`, which makes a browser open its own login window: a route the web page calls takes another scheme.
- Declared in the contract, with a name and a visibility: `pub scheme Key = header "X-Api-Key";`, `pub scheme Session = cookie "session";`, `pub scheme Token = query "token";`, `pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";` (a bearer token on the wire, described as OpenID Connect, whose challenge is `Bearer`; quoll checks no token, `authenticate` does). Each becomes a type of that name implementing `Scheme`, whose credential is a `String` and whose name is its own: the server implements `Authenticate<Key>`. A header or a query key given twice is a 401, as is an empty one. A header scheme's credential is a header of its own, which reqwest, as curl and `fetch`, keeps on a redirect to another host, where it strips only `Authorization`, `Cookie` and `Proxy-Authorization`: a client that sends one follows no redirect to another origin, with the reqwest policy that `rest::client::key` shows. Of a cookie given twice, as a browser holding it for two paths sends it, the first that is not empty is read, the one of the longest path (RFC 6265); an empty one is none, as a cookie cleared by the application. A route that takes a query scheme has no other value of its key: the macro refuses one in the route's string, and a query struct has no field of that name, which the macro cannot see.
- A cookie travels with a request that a page of another site has a browser send, with no preflight for a form or text. A request that carries the cookie of a cookie scheme, with a method other than `GET`, `HEAD` or `OPTIONS`, is answered 403 unless its `Sec-Fetch-Site` is `same-origin` or `none`, or, from a browser that sends none, its `Origin` is its `Host`, compared as origins: one host, and one port, the default of the origin's scheme where either leaves it out. A request with neither header comes from no browser and passes. Behind a reverse proxy, the proxy passes the browser's `Host`, `proxy_set_header Host $host` in nginx; `X-Forwarded-Host`, which anyone may send, is not read. Only the server's own origin is trusted for now: trusted origins come later. The application sets the cookie `SameSite=Lax` or `Strict`, `HttpOnly` and `Secure`, a second defense that does not cover a page of the same site, and names it with the prefix `__Host-`, which a subdomain cannot set.
- Any other is a type of the application implementing `Scheme`, as an HMAC signature, named in `#[auth(...)]` as the others are, by its path: `#[auth(crate::auth::Hmac)]`.

## What the macro generates

Nothing is written to disk: the macro runs as rustc compiles the contract, and its code is compiled in place. `cargo expand` or rust-analyzer's "Expand macro recursively" show it; `cargo doc` shows what is generated, with the contract's comments.

| Item | For | What it is |
|---|---|---|
| `trait NotesServer` | server | one method per route, with the caller its authentication gives |
| `trait Notes` | client | one method per route, implemented by the client and by fakes in tests |
| `struct ApiServices` | server | a field per group: its service |
| `Api::router` | server | an `axum::Router` from an `ApiServices` |
| `Api::client`, `Api::client_with` | client | `api.notes()` is a `NotesClient`, implementing `Notes` |
| `Api::openapi` | both, and neither | the OpenAPI 3.1 document; none with `#[openapi(none)]` |
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
| `basic(user, password)` | `Authorization: Basic`, on the routes whose schemes hold `Basic` only |
| `key::<Key>(value)` | the credential of a scheme the contract declares, where it travels: its header, its cookie after the request's others in one `Cookie` header (a jar's are not added there), its query key, or `Authorization: Bearer` for `openid`; on the routes that take it only |
| `user_agent(name)` | `User-Agent` |
| `request_id()` | a new `X-Request-Id` per call |
| `retry(times)` | backoff on a failure with no response, 502, 503 or 504; `GET`, `HEAD`, `PUT` and `DELETE` only (reqwest-retry) |
| `timeout(duration)` | a deadline per call |
| `trace()` | a tracing span per call, named by the operation id (reqwest-tracing) |

The helpers tell a route's schemes by their type, `rest::Route::takes::<Key>()`, so that two contracts declaring a `Key` each get their own through one `ClientWithMiddleware`, and send nothing on a request with no `rest::Route`. A credential the server would not read, a token that is not a `b64token`, a user with a `:`, a key with a line break, fails each call that would send it with an error that does not show it.

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
| `request_id()` | keeps the caller's `X-Request-Id` or makes one, a UUID v4, and sends it back; a handler reads it with `rest::server::RequestId::current()` |
| `trace()` | a tracing span per request, with the operation id and the request id, and a line per answer (tower-http) |
| `timeout(duration)` | a handler that takes longer answers 503 |
| `body_limit(bytes)` | a larger body answers 413 |
| `cors(origins)?` | CORS for the given origins (tower-http) |
| `compression()` | gzip, brotli and zstd responses, streams aside (tower-http) |

They answer RFC 9457 problems, as handlers do, where tower-http's answer plain text. Any other tower layer applies as is, as a rate limit from tower_governor. Each helper returns a layer type of quoll's, under `rest::server::layer`, which no application names, and no tower-http type.

- **The order.** axum runs the last layer added first: the example adds `body_limit`, `timeout`, `trace`, then `request_id`, so that a request gets its id, then its span, and that a 503 of the deadline and a 413 of the limit are traced and carry the id. Each works in any order.
- **The route.** The router puts the `rest::Route` of a request in the extensions of its answer, where a layer reads it, and names its operation in the span of `trace`.
- **`request_id()`**: the caller's id is kept when it is 1 to 128 bytes of printable ASCII with no space, and replaced otherwise, or when given twice; the request is served. The id is set on the request and its answer, and given to the span. `RequestId::current()` gives it while the handler's future runs, a task-local: not in a task it spawns, nor in the stream it returns.
- **`trace()`**: the span `request` has the `method`, the `path` without the query, which may hold a credential, the `operation` and the `request_id`. Each answer is logged once, `answered` with its status and latency: at `INFO`, and at `ERROR` for a 5xx the crate has not logged with its cause already, as it logs an `Error::internal`, an error of another API and a deadline.
- **`timeout(duration)`**: it bounds the route's future, which reads the body and ends with the head of the answer, and not the body: an `Events` or a `JsonLines` stream goes on past it. The 503 says the deadline, and is logged once.
- **`body_limit(bytes)`**: a `Content-Length` past it is answered at once; a chunked body is read up to it. It sets axum's own limit, through which JSON, `Form`, `Text` and `Bytes` are read, and replaces its default of 2 MiB.
- **`cors(origins)`** takes origins as text, `https://app.example.com`, and returns an error, `rest::server::InvalidOrigin`, for one that is not an origin: `*`, `null`, a path. It allows the methods a contract serves, any header a page asks for, and exposes `Location`, `WWW-Authenticate` and `X-Request-Id`. It allows no credentials: a page of another origin sends a token or a key in a header. A cookie scheme's CSRF check still trusts the server's own origin alone, so that a listed origin's call with the cookie, on a method other than `GET`, `HEAD` and `OPTIONS`, is answered 403.
- **`compression()`** leaves out what tower-http's default does, small answers, images and server-sent events, and JSON Lines too, whose lines would wait in the encoder.

## Errors

- `rest::Result<T>` is `Result<T, rest::Error>`, one error for the server and the client: its `status()`, the RFC 9457 problem when there is one, the raw body, and `body_as::<T>()` for a third-party API's own error shape.
- The server answers a handler's error with its status and a problem in JSON: `Error::not_found(detail)`, `Error::bad_request(detail)`, `Error::unauthorized(detail)`, `Error::new(status, detail)`, and `Error::internal(err)` for a 500 that logs `err` without sending it.
- An error a client received from another API answers 502 when a handler returns it, so that the other API's status is not passed on as ours.
- On the client, a failure with no response (connection, timeout) is an `Error` without a status.
- Every route has this one error, and its TypeScript the problem shape. Typed errors per route, `Result<Note, NotFound>` with `#[derive(rest::Problem)] #[problem(status = 404)]`, come later: adding them breaks no contract.

## OpenAPI and TypeScript

- `Api::openapi()` returns the OpenAPI 3.1 document: paths, parameters, bodies, results, schemas from schemars, security schemes, the problem shape of errors. A binary of the contract crate prints it: the schemas are known only once the types are compiled, which a macro does not see.
- It is a `rest::openapi::Document`: `to_json_pretty()`, `to_json()`, `Serialize`, and `with_title(..)` and `with_version(..)` over its `info`, which is the contract crate's `CARGO_PKG_NAME` and `CARGO_PKG_VERSION`, or the api's name and `0.0.0` for a crate built without Cargo, with the api's `///` as its description.
- An operation is `group_function`, tagged by its group, whose `///` describes the tag. The first paragraph of a route's `///` is its summary, the rest its description.
- Path parameters, query keys and headers are parameters, required unless an `Option` or a `Vec`, never `null`. A query struct is unfolded into one query parameter per field of its schema, or is one parameter `style: form, explode: true`, not required, when its schema is not a plain object. The key of a query scheme is no parameter: the security describes it.
- Bodies: `application/json`, `application/x-www-form-urlencoded` for `Form<T>`, `text/plain` for `Text`, `application/octet-stream` for `Bytes`. Results: 200, 201 with an optional `Location`, 202, 204; `text/plain` for `Text`, `*/*` for `Bytes`.
- Every error is the `Problem` schema, `application/problem+json`. A route lists what quoll answers itself: 400 when it reads an argument, 401 when it takes a credential, 403 when it needs scopes or takes a cookie scheme the contract declares on a method other than `GET`, `HEAD` and `OPTIONS`, 415 when it reads a body, and `default` for any other.
- Schemes sit under `securitySchemes` by their `NAME`. A route's requirement lists its schemes, any of which serves, each with its scopes, and `{}` when its credential is optional.
- Schemas sit under `components/schemas`, as schemars names them: a second type named `Note` in another module is `Note2`, in the order the contract names them, which `#[schemars(rename = "...")]` changes; a character a component's name does not take, a space, a `<`, is a `_`. A parameter or a body is described as serde reads it, a result or a stream's item as serde writes it; a type whose two schemas differ, as with a `#[serde(default)]` field, has both, `NoteInput` and `NoteOutput`, and so has a type that holds one of them. The two sides are paired by schemars' names: two types of one name, one of them used on one side only, may get misleading `Input` and `Output` names, every `$ref` still pointing at the right schema, which `#[schemars(rename = "...")]` avoids.
- A contract of a third-party API that is only called says `#[openapi(none)]` on its api: no `Api::openapi`, and its types, which may come from crates that do not implement `JsonSchema`, need serde alone.
- orval writes the TypeScript from it: TanStack Query here; `vue-query`, `svelte-query`, `swr`, `zod` or a plain `fetch` client from the same document.
- The credential goes through orval's mutator, one function of the application; the browser sends cookies itself.
- A stream's items are typed through an extension, as OpenAPI 3.1 cannot type them: the response's media type (`text/event-stream`, `application/jsonl`) carries `x-item`, a `$ref` to the item's schema, which orval writes as a type like any other. `subscribe<T>(url, onItem, init)`, from `vite-plugin-rest/client`, reads the stream with `fetch`, so that it sends headers, calls `onItem` with each item parsed, and returns the function that stops it. The extension goes once the generators read OpenAPI 3.2's item schemas.
- An `i64` is a `number`, exact up to 2^53: ids and counts stay below it. A value that may go beyond it is declared as a `String` in its Rust type.
- Neither the document nor the TypeScript is committed: the Vite plugin writes them in development and in a build. They are the one generated code on disk, as `tsc` and the editor need files. A test of the example keeps a snapshot of its document, which is no input of the build.

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
    pub scheme Key = header "X-Api-Key";

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
use std::time::Duration;

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
    // The last layer added runs first: a request gets its id, then its span, then its deadline and its body's limit.
    .layer(rest::server::body_limit(1 << 20))
    .layer(rest::server::timeout(Duration::from_secs(10)))
    .layer(rest::server::trace())
    .layer(rest::server::request_id())
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
- Two bodies; a body on `GET`, `HEAD` or `OPTIONS`; a result on `HEAD`, whose answer has no body.
- A path parameter of a type the macro sees is not read from text: a `Vec` or another collection, a map, an `Option`, a reference, a tuple, or `Form`, `Text`, `Bytes` or `Created`; a header of such a type, but an `Option` of a type read from text. A struct of the application cannot be told from an enum of unit variants, and is refused when it is called: the server answers 500, logged, and the client a misuse error, before sending.
- A query struct and a query in the string together; two query structs; a query struct whose type is not a struct: a scalar, a `String`, an `Option`, a `Vec`...
- A header read by two arguments; an argument that is a header or the query struct and a `{name}` of the string too.
- A header that quoll or HTTP writes itself, `Content-Type`, `Content-Length`, `Host`, `Transfer-Encoding` and the hop-by-hop ones, and `Authorization` on a route that takes a scheme.
- One method and path twice in an api.
- A group less visible than its api, whose router and client name the group's items.
- Two items the macro writes with one name: a group named as its api, or as the `NotesServer`, `NotesClient`, `ApiServices` or `ApiClient` the contract writes for another; two routes of one operation id, as `tags_list` in a group `Note` and `list` in a group `NoteTags`, both `note_tags_list`.
- A route left out of a service or a fake, a handler or a fake's method of the wrong signature: Rust's own errors on the trait.
- A group without a service; a service of a group with authentication that does not implement `Authenticate` for its schemes; two schemes of one `#[auth(A | B)]` giving callers of two types; scopes on a caller that does not implement `Scoped`.
- A scheme in `#[auth(...)]` that is neither given by the crate, declared in the contract nor a type implementing `Scheme`; a scheme's credential that is not `Send`; `key::<K>` for a scheme the contract does not declare; two schemes of one `NAME` in an api, which names a scheme in the OpenAPI document, or a `NAME` of other characters than ASCII letters, digits, `.`, `-` and `_`.
- A type of a contract that does not implement `JsonSchema`, unless the api says `#[openapi(none)]`; `#[openapi(...)]` with anything but `none`, or on a scheme, a group or a route.
- In `#[auth(...)]`: `none` beside a scheme or an option; a scheme twice in an alternative, or more than four; a path to a declared scheme, `self::Key`, or through `rest` to the crate's, `rest::Bearer`, which are named alone; two schemes of an alternative that read one place, `Bearer | Login` for an `openid` scheme or two header schemes of one header; `optional` or `scopes` twice, or an option it does not take; `scopes()` empty, a scope written twice or that is not an OAuth `scope-token`; `optional` with `scopes`.
- A declared scheme of a kind other than `header`, `cookie`, `query` and `openid`; named `Bearer`, `Basic` or `none`, with a name that is not ASCII, or with the name of another scheme or of an item the macro writes; a header that quoll or HTTP writes, `Authorization` or `Cookie`; a header or a cookie whose name is not a token, a query key other than letters, digits and `-._~`; an `openid` URL that is not `http(s)://`; a scheme less visible than a group that takes it; `#[auth(...)]` on a scheme.
- An argument that reads the credential of a scheme its route takes: the header of a header scheme, `Cookie` beside a cookie scheme, the query key of a query scheme.

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

Decided, not in the first version. The first two change no contract already written.

1. **Typed errors per route**, as the Errors section says.
2. **Pagination by header.** GitHub pages with a `Link` header (`rel="next"`, `rel="last"`), which a result cannot say yet: a result `Paged<T>` carrying it, and on the client `.pages()`, a stream that follows the links.
3. **A `rest::PathValue` derive** on unit enums and newtypes, so that a path parameter not read from text is refused at compile time: a struct of the application in a path no longer compiles.
