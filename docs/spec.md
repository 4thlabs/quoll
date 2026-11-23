
# The REST contract

> [!NOTE]
> **Validated** by the maintainer on 2026-11-23.

A REST contract written once in Rust, from which a macro generates a server on axum, a client on reqwest and an OpenAPI document; the OpenAPI document feeds TypeScript generators (TanStack Query, SWR, zod...). No UI. It takes the REST spec of the C++ Quoll, now [quoll-http](https://github.com/4thlabs/quoll-http), as its model.

Decided by the maintainer:

- **2026-11-18**:
  - the contract is one function-like macro, `quoll::api! { ... }`;
  - the contract says which authentication a route takes, `#[auth(Bearer)]`, and the server's handler receives the authenticated caller as a typed argument;
  - authentication is generic, through schemes;
  - no generated Rust is written to disk;
  - the client is built on reqwest-middleware and is native only, with no wasm32 for now.
- **2026-11-19**:
  - the crate is named `quoll`;
  - errors stay one `quoll::Error`, typed errors per route come later;
  - the items of a stream are typed in TypeScript through an extension and a `subscribe` helper;
  - an `i64` is a TypeScript `number`;
  - pagination by header comes later;
  - the error of `authenticate` answers its own status, not always 401;
  - a path parameter of a struct type is refused when it is called, until a `quoll::PathValue` derive refuses it at compile time;
  - a header read twice, an argument in two places, a query struct that is not a struct and a header that quoll or HTTP writes do not compile;
  - `Bytes` carries its content type;
  - a failure after the 200 ends an `Events` stream with an `event: problem`, and the client reads every event that has data, whatever its name;
  - a scheme gives its name and its challenge as `Scheme::NAME` and `Scheme::CHALLENGE`, its credential is `Send`, and `openapi` comes with the OpenAPI document;
  - `Basic` reads a `BasicCredential` and challenges with the realm `api`;
  - a declared scheme is written with its visibility, `pub scheme Key`;
  - in `Bearer | Key` the first credential the request carries decides;
  - a route needs all its scopes, and `optional` with `scopes` does not compile;
  - `key::<K>` sends any declared scheme's credential, `openid`'s included, and the client's middleware tell a route's schemes by their type;
  - a cookie scheme refuses a request from another origin, as Go's `CrossOriginProtection` does, the server's own origin alone trusted for now;
  - an alternative takes up to four schemes;
  - `client::redirects(n)` follows redirects one hop at a time through the middleware after it, on a reqwest client that follows none, and comes before every middleware that adds a credential; the helpers send a credential, and `on_request` runs, on the requests of a contract to the api's own origin only;
  - `bearer_from` takes a `client::TokenSource`, whose `token` it asks before each call and whose `refresh` once after a 401, the 401 answering when the token is the same or the body cannot be sent twice;
  - `user_agent` and `request_id` set their header when the request has none, one id across the attempts of a call;
  - `timeout` is a deadline per attempt and per hop, for the head of the answer alone on a stream, which `retry` tries again;
  - `Api::openapi()` returns a `quoll::openapi::Document` and is written whatever the features, so that every type of a contract implements `JsonSchema`, unless the api says `#[openapi(none)]`;
  - `Scheme::openapi` returns a `quoll::openapi::SecurityScheme` and has no default;
  - the document's `info` is the contract crate's name and version, a query struct is unfolded into its keys, `Bytes` are `application/octet-stream` as a body and `*/*` as a result, each route lists the errors quoll answers and a `default`, two types of one name take schemars' suffix, a type read and written otherwise has two schemas, operation ids are unique, two schemes of one `NAME` in an api do not compile, and the notes document is a snapshot of its tests;
  - the server's helpers return quoll's layer types, under `server::layer`, which the example adds from the innermost to the outermost;
  - `request_id()` keeps a caller's id of 1 to 128 bytes of printable ASCII with no space, or makes a UUID v4, which a handler reads with `RequestId::current()`, a task-local;
  - the span of `trace()` has the method, the path without the query, the operation and the request id, and each answer is logged once, at `ERROR` for a 5xx the crate has not logged with its cause;
  - `body_limit` replaces axum's default limit;
  - `cors(origins)?` refuses what is not an origin with an `InvalidOrigin`, allows the methods a contract serves and any header, exposes `Location`, `WWW-Authenticate` and `X-Request-Id`, and allows no credentials, the CSRF check of a cookie scheme still trusting the server's own origin alone.
- **2026-11-20**:
  - a client error's sources show its URL by origin and path, never its credentials nor its query;
  - the helpers move to the top of the crate, `quoll::client` and `quoll::server`, and the router of a test to `quoll::test`, with no re-export left in the module of the contract;
  - `client::builder()` takes quoll's helpers and any `reqwest_middleware::Middleware` with `with`; it puts quoll's own in the order they work in, `trace`, `user_agent`, `request_id`, `retry`, `timeout`, `redirects`, then the credentials and `on_request`, whatever the order they are given in, each other where it is given, and tells a helper by its type; it configures reqwest's client with `http(..)`, follows no redirect in reqwest's client when `redirects` is given, and adds `redirects(10)` when it is given a credential, `on_request` included, and no `redirects`; it builds a contract's client with `build::<Api>(url)`, through the trait `client::Contract` the macro implements on `Api`, or a `ClientWithMiddleware` with `build_http()`;
  - `server::builder()` takes quoll's layers and any tower layer with `with`, the first given the outermost; it puts quoll's own in the order `request_id`, `trace`, `cors`, `compression`, `timeout`, `body_limit`, each other where it is given; it serves a router with `serve(address, router)`, an address a `&str`, a `String` or a `SocketAddr`, or `serve_on(listener, router)` until Ctrl-C or `SIGTERM`, or the signal given to `shutdown(..)`, waits 5 s for the requests it serves, `grace(..)`, or until a second signal, and logs at `WARN` the connections it leaves open; it gives the router with its layers with `into_router(router)`;
  - `Api::client`, `Api::client_with` and `Api::router` stay;
  - `build::<T>` on a type that is not a contract's api does not compile;
  - a contract splits into `quoll::group!` blocks, one group per block, which an api lists by their paths, the usual form, `{ auth::Auth, notes::Notes }`, a group named alone needing its module imported by glob, `use notes::*;`, and rustc's error on the name otherwise;
  - the api reads them through a hidden descriptor, by type, and checks them against each other at compile time, inline groups going through the same generator;
  - schemes are declared in the api alone, a `group!` naming one by its path or an import, the rule that names a declared scheme alone holding in the api's block only;
  - a group may be listed by two apis, each serving it under its prefix;
  - `#[openapi(none)]` goes on a `group!` too;
  - two schemes of different types of one `NAME` from two blocks, which the macro cannot compare, make `Api::openapi()` panic with a message saying so, which the snapshot test of the document catches;
  - the example's store is in memory, without SQLite;
  - for the example of the Docker Engine API: the client reads a `JsonLines` sent as `application/json` too, which it asks for last, `application/json;q=0.5`, the server still sending `application/jsonl`;
  - for the same example, a result `quoll::ByteStream`, bytes of a content type sent as they come, one type on both sides, of chunks that are `Vec<u8>`, `bytes` being no dependency of the API, described as `Bytes` are in the document, `*/*`, a stream for every rule streams follow;
  - the TypeScript chain is driven by a Rust core, `cargo quoll gen`, `dev` and `build`, configured in the workspace's metadata, which runs the commands it names; no plugin for now; `subscribe` comes later.
- **2026-11-21**:
  - with `cargo quoll dev`, its `--json` events are `document`, `error`, `server` and `web`, as [The dev loop](#the-dev-loop) lists them;
  - a server of `cargo quoll dev` runs a copy of its executable, the binary `cargo run` would, and is ready when its url accepts a connection;
  - every child of `cargo quoll dev` runs in a process group or a job, stopped with what it started;
  - an `Events` answer opens with a comment, `: `, and ends cleanly when the server of `server::builder()` is told to stop;
  - what a `JsonLines` or a `ByteStream` says of a stop is open, [#70](https://github.com/4thlabs/quoll/issues/70);
  - each example is a workspace of its own, as an application's is, with the config of `cargo quoll` in its own `Cargo.toml`, the repository's root workspace holding the library's crates alone;
  - `quoll::Response<T>` is the answer of HTTP on both sides, its status, its version, its headers and its body, with nothing declared per route in the contract; the response headers are not in the OpenAPI document;
  - every handler returns `quoll::Result<Response<T>>`, as tonic's do, `Ok(Response::new(note))`, with its headers, `.header(ETAG, tag)`; `Created<T>` and `Accepted<T>` are only written in the contract, to say 201 or 202, a handler of either returning a `Response<T>`, and `Location` is a header as any other, `Created::at` gone;
  - the status of a response is the contract's unless the handler gives another, a success or a redirection, with `.with_status(Status::CREATED)`, `quoll::Status` holding a `2xx` or a `3xx` alone, so that an error's status is refused at compile time and goes through `Err(quoll::Error)`; the document describes the contract's status alone;
  - every route's method on the client returns a `quoll::Call<T>`, the call described and not sent yet, an `IntoFuture`: `.await` gives `quoll::Result<T>`, `.response()` the whole answer, `quoll::Result<Response<quoll::Result<T>>>`, its body the route's value on a success and the error of the answer otherwise, a 304 among them, with its status and its headers and no body; its `Err` is only no usable answer; `.timeout(duration)` gives the call a deadline of its own, in place of the client's `client::timeout`, with its meaning; any future of a `quoll::Result<T>` is a `Call<T>` too, whose `.timeout` does nothing and whose `.response()` gives a 200 with no headers, or the status of its error, so that a fake returns an `async move` block;
  - `client::retry(times)` keeps its rule, and `.when(|attempt: &Attempt| -> Retry)` is one decision for every other case, a `429` and its `Retry-After`, a `POST` with an `Idempotency-Key`, a `500`: `Retry::No`, `Retry::Backoff` or `Retry::After(duration)`; the `Attempt` gives the contract's `Route`, the request, the answer's status and headers or the error, and its number, `transient()` the rule of `retry(times)` alone and `retry_after()` the answer's `Retry-After`, in seconds or as an HTTP date, capped by `.max_wait(..)`; the middleware is quoll's own on backon's backoff, which no public signature shows, and reqwest-retry leaves the dependencies;
  - the shape of the errors another API answers is said once, `#[error(E)]` on the api, and the errors of its calls carry its type: they return `quoll::Result<T, E>`, an alias of `Result<T, quoll::Error<E>>`, `quoll::Error<Problem>` being `quoll::Error`, the default; `error.detail()` gives the `E`, `error.response()` the answer with it; a body that is not an `E` is an answer with no detail, which keeps its status and its raw body; a fake builds one with `quoll::Error::answered(status, e)`;
  - a literal query key may hold `[` and `]`, so that `"/customers/{id}?expand[]={expand}"` and `"/articles?filter[name]={name}&page[size]={size}"`, the keys of Stripe, JSON:API and Rails, are written in the route's string; nothing else changes, the server reading the query through serde; the nested form, `metadata[k]=v` from a map, is no syntax of the contract: its recipe comes with the guide of [#84](https://github.com/4thlabs/quoll/issues/84).
- **2026-11-22**:
  - the body of `.response()` is `quoll::Result<T, E>`: `Ok(T)` on a success, `Err(quoll::Error<E>)` on any other status, with `detail()` when its body is an `E`; a 304 is an `Err` with its status and no `detail()`, its headers those of the `.response()`;
  - `#[error(E)]` is inherited api → group → route, as `#[auth]` is, the nearest to the call winning;
  - `E: Display` gives the message: `another api answered 401: Bad credentials`; rejected, an attribute naming a field;
  - the fixed headers of an api are in its contract, `#[header("name" = value)]` on the api, a group or a route, inherited downward as `#[auth]` is, the nearest to the call winning: the caller's `header("X")` argument, the route, the group, the api, then quoll's own `Accept`; `#[header("X" = none)]` leaves one inherited header and `#[header(none)]` all of them, never what quoll writes for the route; a `none` on a header its parent does not fix does not compile; a `Content-Type` keeps the media type of the body, adding a parameter; the server does not check them, and the document describes each as a header parameter with `const`, not required; `on_request` stays for what belongs to a deployment;
  - taken with it as implementation, written in [Fixed headers](#fixed-headers): a group of another block takes its api's fixed headers, which are values read at run time, as the prefix is, the api checking at compile time what its groups leave and the `Content-Type` they take; a value is a string or the name of a `&str` constant, checked at compile time; a fixed `Content-Type` is left out of a request with no body, a route's own on one refused, refused on a `Bytes` body, which carries its own, and with a charset other than UTF-8, which quoll writes its bodies in; a fixed header is refused where an argument's would be, but `Content-Type`, and with it the credential of a scheme the route takes; `Accept`, `Content-Type` and `Authorization`, which OpenAPI describes otherwise, and a header an argument reads, are not described as fixed.
  - a list is written as the one value of a query key by `#[join(sep)]` on its argument, in the route's line, its type staying the natural one, `#[join(",")] labels: Vec<String>`: `#[join(",")]` is `labels=bug,docs`, `style: form, explode: false`, `#[join(" ")]` `labels=bug%20docs`, `spaceDelimited`, `#[join("|")]` `labels=bug|docs`, `pipeDelimited`, and the server reads the list back; another separator, or an argument that is not a list, does not compile, spanned on the attribute; rejected, `#[query(labels(join = ","))]` on the route, which repeats the argument's name, and a `quoll::Comma<T>` type;
  - a field of a query struct is written in plain serde, the norm: `#[serde(with = "quoll::join::Comma::<u32>")]`, or `quoll::join::Comma` alone; `quoll::join::Comma<T>`, `Space<T>` and `Pipe<T>` are public types, whose `serialize` and `deserialize` serde calls and whose schema schemars reads, carrying the mark the document turns into the parameter's style; their `T` defaults to `quoll::join::Any`, which serde infers from the field either way, and with which the schema of the items says nothing, schemars taking the type written in the string as it is; this replaces `#[quoll::query]`, decided earlier the same day and merged in #92, which is removed; a shorter form for a field comes later, once schemars or a macro of quoll's sees the field;
  - taken with them as implementation, written in [Parameters, by name](#parameters-by-name): the client writes an argument's values escaped, as a path parameter's are, with the separator between them as is, a space as `%20`, and a query struct's field as serde_html_form writes its text, the separator escaped, `labels=bug%2Cdocs`; the server decodes a value before it splits it, so that it reads both, and the `%2C` of a browser's `URLSearchParams`, through which orval's clients write it; so a value cannot hold the separator, which the client refuses, as it refuses a list of one empty value, which reads as an empty list; an empty list leaves the key out; the values are read from text, as a path parameter is; a joined field is its joined text, or `null` when empty, in JSON too, and its schema says so, a string or `null`, with a mark holding the list's own schema, which the document puts in the query parameter it unfolds and removes elsewhere;
  - after #91, the rule on a `none` of a group of another block relaxed: a `#[header("X" = none)]` in a `quoll::group!` leaves `X` where the api that lists the group fixes it and does nothing elsewhere, so that any api may list the group; in the api's own block, where the parent is known, a `none` on a header the parent does not fix stays a compile error, which catches a typo; the four defaults of #91 confirmed.
- **2026-11-23**:
  - a list is joined only on a route's argument, by `#[join(sep)]`, whose type the macro sees and of which nothing reaches the JSON of a body or a result; this replaces the joined field of a query struct decided on 2026-11-22: `#[quoll::query]`, merged in #92, and the serde form that took its place, `quoll::join::Comma<T>`, `Space<T>` and `Pipe<T>`, merged in #94, are both removed, with the mark their schema carried into the document; a `Vec` field of a query struct repeats its key, as before #92; a joined field of a query struct is reopened in #93;
  - taken with the brackets of a query key as implementation, written in [Methods and paths](#methods-and-paths): a key is a name of the characters of a literal segment, then brackets that hold the same or nothing, never `.` or `..`, `expand[]`, `filter[name]`, `a[b][c]`, and a bracket left open, a bracket first or text after one does not compile; the client escapes them, `expand%5B%5D=a`, as serde_html_form and a browser's `URLSearchParams` write a key, and the server reads them escaped or as they are, which curl sends; a query scheme's key keeps its rule, letters, digits and `-._~`.
  - a 204 is a success, never an error, and one rule says where: a 204 or a 205 is allowed or refused at compile time by the type of the handler's value alone, whatever the route's result wraps it in; allowed when the value may be empty, `()`, `Option<T>`, `Text`, `Bytes`, `Events<T>`, `JsonLines<T>` and `ByteStream`, and in a `Created<T>` or an `Accepted<T>`, whose `T` is a JSON value, `()` and `Option<T>`, so that an `Accepted<()>` and a `Created<Option<Note>>` may answer 204, a `Created<Text>`, `Bytes` or stream not compiling, its message on the type, which quoll's client reads as `()`, `None`, an empty text or bytes, a stream that ends at once; refused when it must carry a value, a `Note` of `-> Note`, `-> Created<Note>` or `-> Accepted<Note>`, its message naming the handler's `T`; the OpenAPI document lists a 204 by the same type, in a `Created` or an `Accepted` too, from one list of the types that may be empty; the notes page's mutator gives `null` on a 204 or a 205; the other successes stay allowed everywhere; an api that answers 204 on a route of a value gives the read error of today, the `.response()` keeping its status and its headers, its body that error. This takes the place of the rules decided earlier the same day, `()` and `Option<T>` alone, then a refusal of a `Created<T>` or an `Accepted<T>` of an empty `T`, which broke `Accepted<()>`.
  - taken with it as implementation: `Status::NO_CONTENT` and `Status::RESET_CONTENT` are a `quoll::NoContent`, which `NoContent::try_from(status)` gives from a 204 or a 205 another API answered, and which `Status::try_from` refuses; `with_status` takes a `quoll::IntoStatus`, a `Status` or a `NoContent`, whose body `quoll::Allows<T>` the response's value, the one list of the types that may be empty, which the document reads through a bound on it, so that another argument, an `http::StatusCode`, has a message of its own; a route with no result that answers 201 or 204 gives each in its branch; the value of a `Response` given a 204 is not sent, as a 304's; the client drops the body a 205 carries, its bytes of `application/octet-stream`, and reads a 204 as `null` for an `Option<T>` or `()` alone, told by its type.
  - the fixed fields of a body are in the contract, `#[body("name" = value)]` on a route, as `#[header]` is for headers, which replaces the decision of 2026-11-21 in [#83](https://github.com/4thlabs/quoll/issues/83), a second api `#[openapi(none)]` for the second answer of a method and a path: the client wraps the body in a `Serialize` adapter that adds the fields to the top-level struct or map of `T` as it serializes, the route's codec writing the whole, JSON, `Form<T>` and any serde format quoll may take later, with no intermediate value, a fixed field replacing a field of `T` of its name; the server reads the same bytes twice, into a struct of the fixed fields alone, which picks the route, then into the handler's `T`, which ignores them; two routes of one method and path are allowed when their `#[body]` tell them apart, by a field in common with different values, and do not compile otherwise; a value is a bool, a number or a string; a body serializes as a struct or a map, `Text` and `Bytes` not compiling and another `T` refused when it is sent; the format is self-describing, and a `T` with `#[serde(deny_unknown_fields)]` refuses the field when it is read, with a clear message; a single route's fixed field is a `const` property of its body schema, two routes of one method and path described later as one operation with a `oneOf` and a `discriminator`, compiling until then in an api `#[openapi(none)]` alone; the guide's Anthropic example is one api with `create` and `stream`.
  - taken with it as implementation, written in [Fixed fields of the body](#fixed-fields-of-the-body): two values tell routes apart when they are of one kind, numbers compared as numbers, as a form writes `true` and `"true"` alike; a field is fixed once by a route, a negative number written `-1`, and `#[body]` goes on a route with a body alone; the fields come first in the body, before those of `T`, the length the format is given exact, counted by a first pass over `T` that serializes none of its values, so that MessagePack and CBOR, which write it, read the body back, the `Serialize` of `T` giving the same at each pass, as a derived one does; a key of a map is compared to a field's name as its text, an integer, a bool and a finite float among them, as serde_json writes a key; a route alone on its method and path checks its fields too, after it authenticates, a body without them a 400 that names them, and the document lists them as required; two routes that share them read the body, within its limit, before either authenticates, as it picks the route, so that the 400 of a body that holds the fields of none names no route nor field, and is the same for a body that does not read as them; a content type none of them reads is a 415 that names the media types they read, and one they read with a parameter they refuse, a form's charset, is the first such route's to refuse; a body that does not read as its fields, a mistake of syntax or a field of another type, is a 400 of its own, with the format's message, on a route alone, once authenticated; a field a form gives twice is one the body does not hold, as in JSON; a `T` with `#[serde(deny_unknown_fields)]` gives the 400 of a body that does not read as `T`, serde's message naming the field.
  - the contract, until now the module `quoll::rest`, which the examples brought in with `use quoll::rest`, is at the crate's root, quoll being a REST contract and nothing else: `quoll::api!`, `quoll::group!`, `quoll::Response`, `quoll::Result`, `quoll::Error`, `quoll::Status` and every item the module held, its submodule `quoll::openapi` too; `quoll::client`, `quoll::server` and `quoll::test` stay modules of their own; the module `rest`, which repeated the crate's name, is gone, with no alias, there being no release yet.

## Stack and crates

- **tokio** for the runtime, **axum** (on hyper and tower) for the server, **reqwest** with **reqwest-middleware** for the client. Native only: the browser goes through the TypeScript.
- **serde** and **serde_json** for bodies, **serde_html_form** for forms and queries (it repeats a key for a `Vec`), **schemars** for the JSON Schema of each type, which OpenAPI 3.1 uses as is. Every type a contract names implements `JsonSchema`, whatever the features, as the document is written with neither.
- **syn** and **quote** in the macro.
- The server hands back an `axum::Router`, the client takes a `reqwest_middleware::ClientWithMiddleware`: tower layers and reqwest middleware stay the user's, with no wrapper in between. `client::builder()` and `server::builder()` build them, with quoll's helpers in the order they work in, and give them back: the router with its layers, the `ClientWithMiddleware`.

An application splits into crates by role:

```text
notes/
  api/      notes-api     the contract and its models: quoll, serde, schemars; no tokio, axum or reqwest
  server/   notes-server  notes-api with feature "server": tokio, axum
  cli/      notes-cli     notes-api with feature "client": tokio, reqwest
  web/      TypeScript    vite, orval, TanStack Query
```

The crate is `quoll`, used from git until it is published; the contract is at its root, `quoll::api!`, `quoll::Response`, `quoll::Error`, which the examples name by their path or import. It has two features, `server` and `client`, which bring the modules of each side: `quoll::client`, the builder of a client and its middleware, `quoll::server`, the builder of a server and its layers, and `quoll::test`, a router served for a test. What the macro generates for each side compiles only with its feature: the command line never links axum, nor the server reqwest. A contract crate passes them on, `notes-api = { path = "../api", features = ["server"] }`.

Quoll's own crates are three. The dev loop is a crate of its own, outside `quoll`, which no application depends on: it is installed, with `cargo install --git https://github.com/4thlabs/quoll quoll-cli` until it is published.

| Crate | What it is |
|---|---|
| `quoll` | The contract, at its root, and the `server`, `client` and `test` modules behind their features. |
| `quoll-macros` | The proc macro behind `quoll::api!` and `quoll::group!`, re-exported by `quoll`, never named. |
| `quoll-cli` | The binary `cargo-quoll`, a cargo subcommand: `cargo quoll gen`, `dev` and `build`, as [The dev loop](#the-dev-loop) says. |

## The contract

An api is a block read as a route table: the method, the path, then the Rust function it becomes.

```rust
quoll::api! {
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
- A literal segment and a literal key of the query are ASCII letters, digits, `-`, `_`, `.` and `~`, and not `.` or `..`. A key may end in brackets that hold the same or nothing, as another API names its keys: `"/customers/{id}?expand[]={expand}"`, `"/articles?filter[name]={name}&page[size]={size}"`. The client escapes them, `expand%5B%5D=sources`, as a browser's form does, and the server reads them escaped or not.
- Only the api has a prefix, with no parameter. A parameter that routes share goes in each path: `"/repos/{owner}/{repo}/issues"`.
- The operation id is the group and the function, `notes_get`; the OpenAPI tag is the group.
- `///` comments become the OpenAPI summary and description, and the rustdoc of what is generated.
- A large api splits into `quoll::group! { pub group Notes { ... } }` blocks, one group per block and per module, and an api that lists them by their paths: `pub api Api = "/api" { auth::Auth, notes::Notes };` (see Groups across modules).

### Parameters, by name

An argument is matched to the string by its name, not its position. Each one is:

- **a path parameter**: its name is a `{name}` of the path, read from text through serde as axum's `Path` reads it (integers, strings, UUIDs, unit enums);
- **a query parameter**: its name is a `key={name}` of the query; an `Option` may be missing, a `Vec` repeats the key, or is joined into its one value by `#[join(sep)]`;
- **a query struct**: `query filter: NoteFilter`, whose fields are the keys as serde names them; not with a query in the string; a `Vec` field repeats its key;
- **a header**: `header("Last-Event-ID") last: Option<String>`;
- **the body**: the one argument left, on `POST`, `PUT`, `PATCH` or `DELETE` only; JSON, or `Form<T>`, `Text`, `Bytes`. `Bytes` carries its content type, any media type but the three a browser sends to another site without a preflight (`application/x-www-form-urlencoded`, `multipart/form-data`, `text/plain`), which answer 415, as a missing one does.

A list that another API reads as one value, `labels=bug,docs`, says so on its argument, with `#[join(sep)]`, its type staying a `Vec`:

```rust
pub group Issues {
    GET "/repos/{owner}/{repo}/issues?labels={labels}&state={state}"
        => list(owner: String, repo: String, #[join(",")] labels: Vec<String>, state: Option<State>) -> Vec<Issue>;
}
```

- `#[join(",")]` is `labels=bug,docs`, `style: form, explode: false` in OpenAPI; `#[join(" ")]` is `labels=bug%20docs`, `spaceDelimited`; `#[join("|")]` is `labels=bug|docs`, `pipeDelimited`. No other separator.
- The values are read from text, as a path parameter is. An empty list leaves the key out; a missing key is an empty list, and so is `labels=`.
- The client writes the values escaped, with the separator between them as is. The server decodes the value before it splits it, so that it reads both, and what orval's clients write through `URLSearchParams`. A value cannot hold the separator: the client refuses one, before sending, as it refuses a list of one empty value, which would read as an empty list.
- Only an argument is joined: a `Vec` field of a query struct repeats its key, and a joined field is an open question, #93.

### Fixed headers

An api that asks every request for headers of its own, a version, a media type, says them once, in the contract, which every client of it shares:

```rust
quoll::api! {
    #[openapi(none)]
    #[header("Accept" = "application/vnd.github+json")]
    #[header("X-GitHub-Api-Version" = "2022-11-28")]
    #[error(GitHubError)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: String, repo: String) -> Repository;

        #[header("Accept" = "application/vnd.github.raw+json")]
        GET "/repos/{owner}/{repo}/readme" => readme(owner: String, repo: String) -> Bytes;
    }
}
```

- `#[header("name" = value)]` goes on the api, a group or a route, as many as it needs, one per header, and is inherited downward as `#[auth]` is. The value is a string or the name of a `&str` constant, `#[header("anthropic-version" = ANTHROPIC_VERSION)]`, checked at compile time to be a header's value: visible ASCII, with spaces and tabs inside it only. A value that changes at run time, Stripe Connect's `Stripe-Account`, is a route's argument, `header("Stripe-Account")`, or goes through `on_request`, which stays for what belongs to a deployment, a signature or a tenant.
- The nearest to the call wins: the caller's `header("X")` argument, when it gives one, then the route, the group, the api, then quoll's own, the `Accept` of the route's result. Names are compared ignoring case.
- `#[header("X" = none)]` on a group or a route leaves one header it would take; `#[header(none)]` leaves them all, the headers it names beside it being its own, as `#[auth(none)]` then `#[auth(..)]`. A `none` never removes what quoll writes for the route: `#[header("Accept" = none)]` leaves the api's `Accept`, and the route asks for quoll's again. In the api's own block, where the parent is known, a `none` on a header its parent does not fix does not compile, its message on the name, so that a typo is not silent; neither does a `none` on the api, which has no parent. In a `quoll::group!`, a `none` leaves the header where the api that lists the group fixes it and does nothing elsewhere, so that any api may list the group; a route's `none` on a header its group already leaves does not compile there either.
- `Content-Type` is the body's, which a fixed one keeps, adding a parameter: Slack's `application/json; charset=utf-8` on a JSON body. Another media type does not compile, nor does a charset other than UTF-8, which quoll writes its bodies in. One the route takes of its group or its api is left out of a request with no body; a route's own on a route with none does not compile, nor does any on a `Bytes` body, which carries its content type.
- A fixed header is refused where an argument's would be, the headers quoll or HTTP writes, but `Content-Type`, and on a route that takes a scheme, the header that carries its credential, `Authorization`, a header scheme's, `Cookie` beside a cookie scheme: the client's middleware sends it.
- The values are read at run time, as the prefix is: a group of another block, a `quoll::group!`, takes the fixed headers of the api that lists it, which checks at compile time that the `Content-Type` they take keeps their bodies' media types, and that they carry no credential of their schemes, its message on the group in its list. What they leave is not checked: a `none` does nothing where the api does not fix the header.
- The server does not check them, on a contract of quoll's. The OpenAPI document describes each header a route sends as a header parameter with `const`, not required, so that orval does not ask its caller for it, but `Accept`, `Content-Type` and `Authorization`, which OpenAPI describes otherwise, and a header an argument of the route reads, which the argument describes.

### Fixed fields of the body

An api that tells two answers of one method and path apart by a field of the body, Anthropic's `"stream": true`, says it in the contract, on each route:

```rust
#[auth(ApiKey)]
pub group Messages {
    #[body("stream" = false)]
    POST "/messages" => create(request: MessageRequest) -> Message;

    #[body("stream" = true)]
    POST "/messages" => stream(request: MessageRequest) -> Events<StreamEvent>;
}
```

- `#[body("name" = value)]` goes on a route, one per field, each named once; the value is a bool, a number or a string, a negative number written `-1`. A route with no body, a `Text` or a `Bytes` body, does not compile: the fields go in a body that serializes as a struct or a map, its JSON or a `Form<T>`.
- The client writes the body through a `Serialize` adapter that adds the fields at the top of the struct or the map `T` serializes as, or that an `Option` or a newtype holds, before `T`'s own, and leaves out a field of `T` of the same name: the contract wins, as with `#[header]`. The route's codec writes the whole, with no intermediate value, so that any self-describing serde format takes them; the length the format is given is exact, counted by a first pass over `T` that serializes none of its values, as MessagePack and CBOR write it, so that the `Serialize` of `T` gives the same at each pass, as a derived one does. A `T` that serializes as anything else, a list, a number, an enum's variant under its name, is refused when it is sent, a misuse error, before the request.
- The server reads the same bytes twice: once for the fixed fields alone, which picks the route, then into the handler's `T`, which ignores them. A body holds a route's fields when each is there once, of its value; one that does not read as them, a mistake of syntax or a field of another type, answers 400 with the format's message, once the route has authenticated the request. A `T` with `#[serde(deny_unknown_fields)]` refuses them: the server answers the 400 of a body that does not read as `T`, whose message names the field.
- Two routes of one method and path are allowed when a field both fix has two values of one kind, numbers compared as numbers: `true` and `false`, `1` and `2`, but not `true` and `"true"`, which a form writes alike. Otherwise, or when one of them fixes none, they do not compile, whichever blocks their groups come from. Such routes read the body, within its limit, before either authenticates, as it picks the route: a body that holds the fields of none, or does not read as them, answers one 400, which names no route nor field, as the request is not authenticated yet. A content type none of them reads answers 415, naming the media types they read; one they read with a parameter they refuse, a form's charset other than UTF-8, is refused by the first route that reads it, with its message. A route alone on its method and path checks its fields after it authenticates, as it reads its body, a body without them a 400 that names them.
- The OpenAPI document describes a route's fields as `const` properties of its body, required: an `allOf` of the schema of `T` and of an object that holds them. Two routes of one method and path are described later, as one operation with a `oneOf` and a `discriminator`: until then, they compile only in an api `#[openapi(none)]`, or in a `quoll::group!` that says it.

### Results

| The route returns | Status | On the wire |
|---|---|---|
| nothing (no `->`) | 204 | no body |
| `T` | 200 | JSON |
| `Created<T>` | 201 | JSON |
| `Accepted<T>` | 202 | JSON |
| `Text` | 200 | `text/plain` |
| `Bytes` | 200 | its content type, `application/octet-stream` by default |
| `Events<T>` | 200 | server-sent events, one JSON value each |
| `JsonLines<T>` | 200 | one JSON value per line |
| `ByteStream` | 200 | bytes as they come, of its content type, `application/octet-stream` by default |

The status is the one of this table, unless the handler gives its `Response` another, a success or a redirection (see The server). On the client, a `Created<T>` or `Accepted<T>` route returns the `T`. A `Bytes` result is read whatever its content type, which it keeps; with none, it is `application/octet-stream`. A `Text` result is read from `text/plain` in UTF-8 only. A `JsonLines<T>` result is read from `application/jsonl`, `application/x-ndjson`, `application/x-jsonlines` and `application/json`, which some APIs send their lines as, the Docker Engine API among them, asked for last, `application/json;q=0.5`; the server sends `application/jsonl`.

`Events<T>` and `JsonLines<T>` are one type on both sides: the server builds one from a `Stream` of values, the client gets a `Stream` of `Result<T>`. A failure after the 200 ends an `Events` stream with an `event: problem`, whose data is the problem, which the client reads as that error; the client reads every event that has data, whatever its name. An `Events` answer opens with an empty comment, `: `, sent with its head before the handler's stream gives anything, so that a proxy that holds the head of an answer until its first bytes, as Vite's does, lets it through at once: the client sees the stream open before its first event, and skips the comment as it skips every comment. `JsonLines` and `ByteStream` have no comment to send: such a proxy holds their head until their first line or chunk. A server of `server::builder()` told to stop ends each `Events` stream in flight cleanly, after the event it is sending; a `JsonLines` or a `ByteStream` runs on until the grace has passed, then is cut, which the client reads as an error (see The server).

`ByteStream` is one type on both sides too, for bytes that are not JSON and come as they are made, a process's output or a live feed: the server builds one from a `Stream` of chunks, `Vec<u8>`, with `new` or `try_new`, of its content type, `with_content_type`, sent with `nosniff` as `Bytes` are; the client asks for `*/*`, reads an answer of any content type, which `content_type()` gives, and gets a `Stream` of `Result<Vec<u8>>`, the chunks as they come. It is a stream as the others are: `timeout` bounds its head alone, which `retry` tries again, `compression()` leaves it out, dropping the client's stream closes the connection, and a failure after the 200 cuts the body short, as JSON Lines have it.

### Groups across modules

```rust
// notes.rs
use quoll::Created;
use crate::{Key, NewNote, Note};

quoll::group! {
    /// The signed-in user's notes.
    #[auth(Bearer | Key)]
    pub group Notes {
        GET  "/notes/{id}" => get(id: i64) -> Note;
        POST "/notes"      => create(note: NewNote) -> Created<Note>;
    }
}

// lib.rs
mod auth;
mod notes;

quoll::api! {
    /// Notes, kept per user.
    pub api Api = "/api" { auth::Auth, notes::Notes };

    /// The key of an operator's scripts.
    pub scheme Key = header "X-Api-Key";

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> Stats;
    }
}
```

- A `quoll::group!` holds one group, written as in an api, which becomes the same items in its module: `NotesServer`, `Notes` and `NotesClient`. It has no prefix, no api and no scheme of its own.
- The api lists its groups by their paths, after its prefix: their routes are under its prefix, their services fields of `ApiServices`, their clients methods of `ApiClient`, their routes in its document. Paths are the usual form. A group named alone, `{ Notes }`, is one in the api's module or block, or brought by a glob import, `use notes::*;`, which brings its items; otherwise rustc finds no descriptor of the group, `__QuollGroupNotes`, for the name, an error on it. The groups of the list come first, then the ones the api's block writes, which may sit beside them.
- A `quoll::group!` takes the fixed headers of the api that lists it, `#[header(...)]`, values read at run time; any api may list it, a `none` of the group's doing nothing where that api does not fix the header (see Fixed headers).
- Schemes are declared in the api alone. A `group!` names one by its path, `#[auth(crate::Key)]`, or by an import, as any type implementing `Scheme`: the rule that names a declared scheme alone holds in the api's block.
- The api reads the routes of a group through a hidden descriptor the group writes, `__QuollGroupNotes`, by type, and checks them at compile time against its own and those of its other groups: one method and path, one path with its parameters named otherwise, one operation id, a group less visible than its api, a declared scheme less visible than a group that takes it, two schemes of a route that read one place, an argument that reads the credential of a scheme its route takes. A scheme says by its type whether it is a declared one, with its place, its visibility and whether it checks the origin of a request: a scheme of the application's of a declared scheme's `NAME` is not taken for it. The error points at the group in the api's list. A group the api's block writes goes through the same generator, its mistakes still pointed at their tokens; one that takes a scheme of another block, which the macro knows by its type alone, has what its arguments and its fixed headers read checked so too, the error on its name.
- A group may be listed by two apis: each serves it, calls it and describes it under its own prefix.
- A `group!` has an OpenAPI document, for which its types implement `JsonSchema`, unless it says `#[openapi(none)]`; an api with a document lists only groups that have one.
- Two schemes of different types and one `NAME` from two blocks, which the macro cannot compare, make `Api::openapi()` panic with a message that names them: the snapshot test of the document catches it.

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
    async fn get(&self, user: &User, id: i64) -> Result<Response<Note>> { ... }
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
- With scopes, the caller implements `quoll::Scoped`, and the router answers 403 before the handler, after `authenticate` and before the path, the query and the body are read, when the caller lacks one of them: a route needs every scope it names, `scopes("repo", "admin")` both. `optional` with `scopes` does not compile: a request with no credential has no scope to check. OpenAPI lists each route's scopes.
- The client's trait has no caller: its middleware sends the credential.

An implementation writes `authenticate` as an `async fn`, as the server above does.

```rust
pub trait Authenticate<S: Scheme>: Send + Sync + 'static {
    type Caller: Send + Sync + 'static;

    fn authenticate(&self, credential: S::Credential) -> impl Future<Output = Result<Self::Caller>> + Send;
}

pub trait Scoped {
    fn has_scope(&self, scope: &str) -> bool;
}
```

### Schemes

A scheme is a type implementing `quoll::Scheme`: how the server reads a credential, its name, the challenge of a 401, and how OpenAPI describes it.

```rust
pub trait Scheme: 'static {
    type Credential: Send + 'static;

    const NAME: &'static str;
    const CHALLENGE: HeaderValue = HeaderValue::from_static(Self::NAME);

    fn read(request: &http::request::Parts) -> Result<Option<Self::Credential>>;
    fn openapi() -> SecurityScheme;
}
```

- `NAME` is what `quoll::Route::schemes()` holds and what OpenAPI names the scheme; `CHALLENGE` is the `WWW-Authenticate` of a 401, a constant, which the macro evaluates for each scheme a route takes, so that one that is not a header's value does not compile. The credential is `Send`, as `authenticate` holds it across an `await`. `openapi` describes the scheme in the document, with no default: `SecurityScheme::bearer()`, `basic()`, `http(name)`, `header(name)`, `cookie(name)`, `query(name)` or `openid(url)`, `.with_description(text)`. The crate's schemes and the declared ones give theirs, a declared one with its `///` as the description.
- `read` gives `None` for a request with none of its credential, leaving to another scheme an `Authorization` header of another scheme, and a 401 for one of its own that is not well formed. Its detail never shows the credential.
- Given by the crate: `Bearer`, whose credential is the token, a `String`; and `Basic`, whose credential is a `quoll::BasicCredential { user, password }`, read from the base64 of `user:password` in UTF-8, and whose `Debug` hides the password. `Basic` challenges with `Basic realm="api", charset="UTF-8"`, which makes a browser open its own login window: a route the web page calls takes another scheme.
- Declared in the contract, with a name and a visibility: `pub scheme Key = header "X-Api-Key";`, `pub scheme Session = cookie "session";`, `pub scheme Token = query "token";`, `pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";` (a bearer token on the wire, described as OpenID Connect, whose challenge is `Bearer`; quoll checks no token, `authenticate` does). Each becomes a type of that name implementing `Scheme`, whose credential is a `String` and whose name is its own: the server implements `Authenticate<Key>`. A header or a query key given twice is a 401, as is an empty one. A header scheme's credential is a header of its own, which reqwest, as curl and `fetch`, keeps on a redirect it follows to another host, where it strips only `Authorization`, `Cookie` and `Proxy-Authorization`: a client that sends one follows redirects with `client::redirects(n)`, which sends each hop through the middleware after it, on a reqwest client that follows none. Of a cookie given twice, as a browser holding it for two paths sends it, the first that is not empty is read, the one of the longest path (RFC 6265); an empty one is none, as a cookie cleared by the application. A route that takes a query scheme has no other value of its key: the macro refuses one in the route's string, and a query struct has no field of that name, which the macro cannot see.
- A cookie travels with a request that a page of another site has a browser send, with no preflight for a form or text. A request that carries the cookie of a cookie scheme, with a method other than `GET`, `HEAD` or `OPTIONS`, is answered 403 unless its `Sec-Fetch-Site` is `same-origin` or `none`, or, from a browser that sends none, its `Origin` is its `Host`, compared as origins: one host, and one port, the default of the origin's scheme where either leaves it out. A request with neither header comes from no browser and passes. Behind a reverse proxy, the proxy passes the browser's `Host`, `proxy_set_header Host $host` in nginx; `X-Forwarded-Host`, which anyone may send, is not read. Only the server's own origin is trusted for now: trusted origins come later. The application sets the cookie `SameSite=Lax` or `Strict`, `HttpOnly` and `Secure`, a second defense that does not cover a page of the same site, and names it with the prefix `__Host-`, which a subdomain cannot set.
- Any other is a type of the application implementing `Scheme`, as an HMAC signature, named in `#[auth(...)]` as the others are, by its path: `#[auth(crate::auth::Hmac)]`.

## What the macro generates

Nothing is written to disk: the macro runs as rustc compiles the contract, and its code is compiled in place. `cargo expand` or rust-analyzer's "Expand macro recursively" show it; `cargo doc` shows what is generated, with the contract's comments.

| Item | For | What it is |
|---|---|---|
| `trait NotesServer` | server | one method per route, with the caller its authentication gives, returning a `quoll::Response` |
| `struct __QuollGroupNotes` | both, and neither | hidden: the routes of the group, which the api that lists it reads |
| `trait Notes` | client | one method per route, returning a `quoll::Call`, implemented by the client and by fakes in tests |
| `struct ApiServices` | server | a field per group: its service |
| `Api::router` | server | an `axum::Router` from an `ApiServices` |
| `Api::client`, `Api::client_with` | client | `api.notes()` is a `NotesClient`, implementing `Notes` |
| `impl client::Contract for Api` | client | what `client::builder().build::<Api>(url)` builds the client with |
| `Api::openapi` | both, and neither | the OpenAPI 3.1 document; none with `#[openapi(none)]` |
| `struct Key`, ... | both | each scheme the contract declares |

The generated code stays thin and calls the crate, compiled once. A client method, for `GET "/notes/{id}" => get(id: i64) -> Note`:

```rust
impl Notes for NotesClient {
    fn get(&self, id: i64) -> impl quoll::Call<Note> {
        // The macro writes the call: the api's prefix and fixed headers, read at run time, as a group may be listed
        // by two apis, the path with its segment, then the call that sends the request and reads its JSON once
        // awaited, through `quoll::__private`.
    }
}
```

A server trait's method returns `impl Future<Output = quoll::Result<quoll::Response<Note>>> + Send`, which axum needs; an implementation writes `async fn`. A client trait's returns `impl quoll::Call<Note>`: a fake writes `fn get(&self, id: i64) -> impl quoll::Call<Note>` and an `async move` block, as an `async fn` there compiles but makes rustc warn `refining_impl_trait`, which `-D warnings` refuses.

## The server

- A service implements the server trait of its group; one type may implement several.
- Every handler returns `quoll::Result<Response<T>>`, `T` the route's value: `Note` for `-> Note`, `-> Created<Note>` and `-> Accepted<Note>`, `()` for a route with no result, the stream for a stream. `Ok(Response::new(note))` answers the contract's status, `.header(name, value)` adds a header, after those of its name, a `Location` as any other, and `.with_status(Status::..)` gives a success or a redirection in place of the contract's: a `PUT` that answers 201 when it creates and 200 when it replaces, a 304 Not Modified, a 206. `quoll::Status` holds a `2xx` or a `3xx` alone, `TryFrom<StatusCode>` refusing the others with an `InvalidStatus`: an error's status is an `Err(quoll::Error)`, which answers its problem. A 204 or a 205, `Status::NO_CONTENT` and `Status::RESET_CONTENT`, a `quoll::NoContent`, is given to a value that may be empty alone, `()`, an `Option<T>`, a `Text`, `Bytes` or a stream, whatever the result wraps it in, an `Accepted<()>` or a `Created<Option<Note>>` among them, and does not compile on a value that must be one, a `Note` of `-> Note`, `-> Created<Note>` or `-> Accepted<Note>`, its message saying to write an `Option`. A 204, a 205 or a 304 is answered without its body, which the `Response` still holds, and without the headers that describe one.
- A handler's header replaces one quoll puts of its name, a stream's `Cache-Control`; a name or a value that is not one, and a `Content-Type`, a `Content-Length` or a `Transfer-Encoding`, which quoll writes from the result, answer 500, logged without the value.
- A service of a group with authentication also implements `Authenticate` for its schemes (see Authentication).
- `ApiServices` has one field per group: a missing one does not compile.
- A service is `Send + Sync + 'static`; the router holds it in an `Arc`. Its state is its own fields: there is no axum `State` to thread through.
- `server::builder()` serves the router: `.with(layer)` for quoll's layers and any tower layer (see Middleware), `.serve("0.0.0.0:8080", router)` binds an address, a `&str` or a `String` whose host is looked up without blocking, or a `SocketAddr`, and serves, `.serve_on(listener, router)` serves a listener bound already, a `TcpListener` on port 0 or a `UnixListener`. Each returns an `std::io::Result<()>`, an error when the address cannot be bound or the signals cannot be listened to.
- It serves until Ctrl-C, or `SIGTERM` on Unix, which `docker stop` and Kubernetes send, or the future given to `.shutdown(signal)`, `std::future::pending()` for never. Told to stop, it accepts no more connections and returns once the requests it serves have ended, or after its grace, 5 s, `.grace(duration)`, or at a second Ctrl-C or `SIGTERM`. Then it logs at `WARN` that connections were left open, and returns `Ok` all the same: they end with the runtime, as `main` returns, and their handlers may run until then. Ctrl-C and `SIGTERM` stay tokio's once it has returned, their default action not given back.
- Told to stop, it ends the `Events` streams in flight at once, before the grace runs: each ends after the event it is sending, as when the handler's stream ends, so that the client, and any proxy between, reads a clean end rather than a connection cut. It ends with nothing more, no `event: problem`: a stop is no failure, and the client reads it as it reads any end. `EventSource` follows it by connecting again, which brings the stream back only where the server, or a proxy before it, answers that request with a 200: one that answers a 5xx, as Vite's proxy does while the server restarts, closes the `EventSource` for good. A stream that ends at a stop cannot be told from a finite one that completes, a feed of progress: a mark of the end by a stop is the open question of [#70](https://github.com/4thlabs/quoll/issues/70). An `Events` stream whose handler returns once the server is told to stop, or that opens then, ends at once: a 200 with an empty body, read as a clean end. A router served otherwise, by `axum::serve` or `quoll::test::spawn`, leaves its `Events` to run until their connection closes.
- A `JsonLines` or a `ByteStream` in flight is not ended: a clean end would make a finite one, an export or a file, cut by the stop look complete. It runs on as any request the server serves, until the grace has passed, then is cut with its connection as the runtime ends, which the client reads as an error. What a `JsonLines` or a `ByteStream` should say of a stop, an error, a clean end or a choice per route, is open: [#70](https://github.com/4thlabs/quoll/issues/70).
- `.into_router(router)` gives the router with its layers, for an application that serves it itself, or `quoll::test::spawn`.

## The client

- `Api::client(url)` makes a client with no middleware; `Api::client_with(http, url)` takes a `ClientWithMiddleware`.
- `client::builder()` builds one with middleware: `.with(middleware)` for quoll's helpers and any `reqwest_middleware::Middleware` (see Middleware), `.http(|http| http.proxy(proxy))` configures reqwest's `ClientBuilder`, and `.build::<Api>(url)` gives the `ApiClient`, through `client::Contract`, which the macro implements on `Api`: its `Client` and its `client_with`. `.build_http()` gives the `ClientWithMiddleware`, for the clients of several contracts. Both return a `quoll::Result`, an error when reqwest cannot build its client or the URL is not an api's.
- `api.notes().get(3)`: a method per route, arguments in the contract's order. Rust has no named arguments: a route with many optional filters takes a query struct with `Default`.
- Each method returns a `quoll::Call<T>`, the call described and not sent yet, which `.await` sends: it gives `quoll::Result<T>`, the value, or an error that goes up, an answer with an error status among them. Its methods give the call changed: `.timeout(duration)`, a deadline for this call alone, in place of the client's `client::timeout`, with its meaning, a deadline per attempt and per hop, for the head of the answer alone on a stream, which `retry` tries again; a client of `client::builder()` or of `Api::client` keeps it per attempt whatever its helpers, a chain built otherwise without `client::timeout` for the whole call on a stream.
- `.response()` sends it and gives the whole answer, `quoll::Result<Response<quoll::Result<T, E>>, E>`, as reqwest's `send()` gives an `Ok` whatever the status: its status, its version, its headers, a rate limit's or a `Retry-After`, and its body, `Ok(T)` on a success and `Err(quoll::Error<E>)` on any other status, which holds its status, its detail when the body is an `E`, and its body. A 304 is such an `Err`, with no detail: it comes only to a caller that sent `If-None-Match`, whose answer's headers are those of the `.response()`. `response.into_value()` gives the value, or that error, to go up with `?`. A 204 or a 205 is a success, read as `()`, `None`, an empty text or bytes, or a stream that ends at once: on a route `-> T`, as another API may answer it, the body of the `.response()` is the read error, which keeps its status and its headers. The `Err` of `.response()` is only no usable answer: a connection, a timeout, an answer with a success status but a 204 or a 205 that is not the route's result.

```rust
let response = api.notes().get(3).timeout(Duration::from_secs(5)).response().await?;
let remaining = response.headers().get("x-ratelimit-remaining").cloned();

if response.status() == StatusCode::NOT_FOUND {
    return Ok(None);
}

Ok(Some(response.into_value()?))
```

- Any future of a `quoll::Result<T, E>` is a `Call<T, E>` too: a fake returns an `async move` block, whose `.timeout` does nothing and whose `.response()` gives a 200 with no headers, or an answer of its error's status, an error with none, which got no answer, as it is. A fake of an api with `#[error(E)]` builds the error of an answer with `quoll::Error::answered(StatusCode::NOT_FOUND, e)`, and one of a status alone, with no detail, with `quoll::Error::not_found(..).into_shape()`, which turns a `quoll::Error` into one of another shape, leaving its problem. Code generic over `impl Notes` calls either.
- Each request carries a `quoll::Route` in its extensions: the operation id, the group, the method, the path as written and the schemes it accepts. Middleware reads it.

## Middleware

One mechanism a side: reqwest-middleware on the client, tower layers on the server. The crate gives the usual ones, built on the existing crates where there is one.

The builders put quoll's own in the order they work in, whatever the order they are given in: they are sorted into the places they were given at, and any other middleware or layer keeps its place. They tell a helper by its type: one wrapped in a middleware of the application's is the application's. Given to `reqwest_middleware::ClientBuilder` or `Router::layer` directly, each helper works as well, in the order it is added.

### Client, `quoll::client::`

| Helper | What it does |
|---|---|
| `on_request(f)` | a closure that changes each request of a contract to its api's own origin before it leaves, given its `quoll::Route`: what an interceptor does elsewhere |
| `bearer(token)` | `Authorization: Bearer`, on the routes whose schemes hold `Bearer` only |
| `bearer_from(source)` | asks a `client::TokenSource` for a token before each call, and once more after a 401, for tokens that expire |
| `basic(user, password)` | `Authorization: Basic`, on the routes whose schemes hold `Basic` only |
| `key::<Key>(value)` | the credential of a scheme the contract declares, where it travels: its header, its cookie after the request's others in one `Cookie` header (a jar's are not added there), its query key, or `Authorization: Bearer` for `openid`; on the routes that take it only |
| `user_agent(name)` | `User-Agent`, on a request that has none |
| `request_id()` | a new `X-Request-Id` per call, on a request that has none: added before `retry`, the attempts of a call share it |
| `retry(times)` | backoff on a failure with no response, 502, 503 or 504; `GET`, `HEAD`, `PUT` and `DELETE` only; `.when(\|attempt\| ..)` decides otherwise, `Retry::No`, `Backoff` or `After(duration)`, from the `client::Attempt`: its `Route`, its request, the answer's status and headers or the error, its number, today's rule as `transient()` and the answer's `Retry-After` as `retry_after()`, a date measured from the answer's `Date`, capped by `.max_wait(..)` (backon's backoff) |
| `timeout(duration)` | a deadline per attempt and per hop, to the end of the body; on a route whose result is a stream, for the head of its answer alone, which `retry` tries again; a call's own `.timeout` replaces it |
| `trace()` | a tracing span per call, named by the operation id (reqwest-tracing) |
| `redirects(n)` | follows up to `n` redirects, one hop at a time through the middleware after it, on a reqwest client built with `redirect(Policy::none())` |

The helpers tell a route's schemes by their type, `quoll::Route::takes::<Key>()`, so that two contracts declaring a `Key` each get their own through one `ClientWithMiddleware`, and send nothing on a request with no `quoll::Route`, nor to another origin than the api's: the client puts the api's origin in each request's extensions beside its `Route`, and `on_request` runs on the same requests. A credential the server would not read, a token that is not a `b64token`, a user with a `:`, a key with a line break, fails each call that would send it with an error that does not show it.

A header computed from the request is a closure given to `on_request`; the helpers of authentication are written so:

```rust
let sign = client::on_request(move |request: &mut Request, route: &quoll::Route| {
    let body = request.body().and_then(|body| body.as_bytes()).unwrap_or_default();
    let signature = key.sign(request.method(), request.url().path(), body);

    request.headers_mut().insert("X-Signature", signature);
});
```

A full `Middleware`, which wraps the call with `next.run(request)`, is for what also sees the response: a retry, a token refreshed after a 401, a span around the call.

The token source of `bearer_from` is a trait with two `async fn`:

```rust
pub trait TokenSource: Send + Sync + 'static {
    fn token(&self) -> impl Future<Output = Result<String>> + Send;
    fn refresh(&self, refused: &str) -> impl Future<Output = Result<String>> + Send;
}
```

A call answered 401 is sent once more with the token `refresh` gives, and that second answer is the call's; it is not sent again when `refresh` gives back the token refused, or when its body cannot be sent twice. Two calls refused at once each call `refresh` with the token they sent: a source that keeps one token renews it once, under a lock, and gives the other call the new one. An error of the source fails the call.

`redirects(n)` follows 301, 302 and 303 with a `GET` without the body, a `HEAD` staying one, and 307 and 308 with the method and the body, which a route's can always send twice; past `n` hops, or back to a request already sent, the call fails. It comes before every middleware that adds a credential, `on_request` included, so that they run on each hop and send nothing to another origin. A hop to another origin loses `Authorization`, `Cookie`, `Proxy-Authorization` and every header marked sensitive that a middleware before it added, and keeps any other: a credential's header added before it is marked sensitive. Any other answer, or a redirect it does not follow, is the call's answer. Given `redirects`, `client::builder()` builds reqwest's client with `redirect(Policy::none())`, whatever `http(..)` sets. Given a credential, `bearer`, `bearer_from`, `basic`, `key` or `on_request`, and no `redirects`, it adds `redirects(10)` itself, reqwest's own limit, so that redirects are still followed but no hop to another origin carries the key of a header scheme, or what `on_request` adds, as reqwest's own policy would; a `redirects(n)` given stays as it is. Without either, reqwest follows redirects as it does by default. `Api::client` and `Api::client_with` keep reqwest's client as it is.

The order `client::builder()` puts the helpers in, from the first, which sees the request first: `trace`, one span around the whole call; `user_agent`; `request_id`, before `retry`, so that the attempts of a call share one id; `retry`; `timeout`; `redirects`, before every credential; then the credentials and `on_request`, in the order given. It is the order to add them in to a `reqwest_middleware::ClientBuilder`. A middleware of the application's that adds a credential is given after `redirects`.

### Server, `quoll::server::`

| Helper | What it does |
|---|---|
| `request_id()` | keeps the caller's `X-Request-Id` or makes one, a UUID v4, and sends it back; a handler reads it with `server::RequestId::current()` |
| `trace()` | a tracing span per request, with the operation id and the request id, and a line per answer (tower-http) |
| `timeout(duration)` | a handler that takes longer answers 503 |
| `body_limit(bytes)` | a larger body answers 413 |
| `cors(origins)?` | CORS for the given origins (tower-http) |
| `compression()` | gzip, brotli and zstd responses, streams aside (tower-http) |

They answer RFC 9457 problems, as handlers do, where tower-http's answer plain text. Any other tower layer applies as is, as a rate limit from tower_governor. Each helper returns a layer type of quoll's, under `server::layer`, which no application names, and no tower-http type: the builder tells them by it.

- **The order.** `server::builder()` runs the first layer given first, the outermost, and puts quoll's in this order, from the outermost: `request_id`, `trace`, `cors`, `compression`, `timeout`, `body_limit`. So a request gets its id, then its span, and a 503 of the deadline and a 413 of the limit are traced, carry the id and the headers of CORS, which a page needs to read them. `Router::layer` runs the last layer added first: there, the layers are added in the reverse order. In another order each layer still works, but an answer of a layer outside `request_id` carries no id, one of a layer outside `trace` is not traced, past its deadline a request whose `timeout` is outside `trace` has its span dropped with the route's future, and an answer of a layer outside `cors` carries no header of CORS.
- **The route.** The router puts the `quoll::Route` of a request in the extensions of its answer, where a layer reads it, and names its operation in the span of `trace`. An answer no route made carries none: axum's 404 and 405, a 503 of `timeout`, an early 413 of `body_limit`.
- **`request_id()`**: the caller's id is kept when it is 1 to 128 bytes of printable ASCII with no space, and replaced otherwise, or when given twice; the request is served. The id is set on the request and its answer, and given to the span. `RequestId::current()` gives it while the handler's future runs, a task-local: not in a task it spawns, nor in the stream it returns.
- **`trace()`**: the span `request` has the `method`, the `path` without the query, which may hold a credential, the `operation` and the `request_id`. Each answer is logged once, `answered` with its status and latency: at `INFO`, and at `ERROR` for a 5xx the crate has not logged with its cause already, as it logs an `Error::internal`, an error of another API and a deadline.
- **`timeout(duration)`**: it bounds the route's future, which reads the body and ends with the head of the answer, and not the body: an `Events`, a `JsonLines` or a `ByteStream` goes on past it. The 503 says the deadline, and is logged once.
- **`body_limit(bytes)`**: a `Content-Length` past it is answered at once; a chunked body is read up to it. It sets axum's own limit, through which JSON, `Form`, `Text` and `Bytes` are read, and replaces its default of 2 MiB.
- **`cors(origins)`** takes origins as text, `https://app.example.com`, and returns an error, `server::InvalidOrigin`, for one that is not an origin: `*`, `null`, a path. It allows `GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE` and `OPTIONS`, the methods a contract can serve, any header a page asks for, and exposes `Location`, `WWW-Authenticate` and `X-Request-Id`. It allows no credentials: a page of another origin sends a token or a key in a header. A cookie scheme's CSRF check still trusts the server's own origin alone, so that a listed origin's call with the cookie, on a method other than `GET`, `HEAD` and `OPTIONS`, is answered 403. A preflight is an `OPTIONS` request with an `Origin` and an `Access-Control-Request-Method`: any other `OPTIONS` request reaches its route.
- **`compression()`** leaves out what tower-http's default does, small answers, images and server-sent events, and JSON Lines too, whose lines would wait in the encoder, and the answer of every route whose result is a stream, a `ByteStream` of any content type among them.

## Errors

- `quoll::Result<T>` is `Result<T, quoll::Error>`, one error for the server and the client: its `status()`, its `detail()`, the RFC 9457 problem when the body of an answer is one, `response()`, that answer with its headers, the raw `body()`, and `body_as::<T>()` for the extension members of a problem.
- The server answers a handler's error with its status and a problem in JSON: `Error::not_found(detail)`, `Error::bad_request(detail)`, `Error::unauthorized(detail)`, `Error::new(status, detail)`, and `Error::internal(err)` for a 500 that logs `err` without sending it.
- An error a client received from another API answers 502 when a handler returns it, so that the other API's status is not passed on as ours.
- On the client, a failure with no response (connection, timeout) is an `Error` without a status. Its sources, the errors of quoll's client and middlewares and those of reqwest and reqwest-middleware, show the URL of the call by its origin and its path, never its credentials nor its query, where a query scheme sends its key, and so does a reqwest error given to `Error::internal`; an error the application wraps itself is its own to redact, with `reqwest::Error::without_url`.
- Every route of quoll's server has this one error, and its TypeScript the problem shape. Typed errors per route, `Result<Note, NotFound>` with `#[derive(quoll::Problem)] #[problem(status = 404)]`, come later: adding them breaks no contract.

### Another API's errors

Another API answers its errors in a shape of its own. The contract says it once, `#[error(E)]` on the api, and the errors of its calls carry its type:

```rust
/// What GitHub answers on an error.
#[derive(Debug, Deserialize)]
pub struct GitHubError {
    pub message: String,
    pub documentation_url: Option<String>,
}

impl fmt::Display for GitHubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

quoll::api! {
    #[openapi(none)]
    #[error(GitHubError)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: String, repo: String) -> Repository;

        #[error(OAuthError)]
        POST "/login/oauth/access_token" => token(code: String) -> Token;
    }
}
```

- `quoll::Error<E>` is the error of a call whose route has the shape `E`, and `quoll::Result<T, E>` an alias of `Result<T, quoll::Error<E>>`; `quoll::Error` is `quoll::Error<Problem>`. Each method of the group's trait returns `impl quoll::Call<T, E>`.
- `#[error(E)]` goes on the api, a group or a route, and is inherited downward as `#[auth]` is, the nearest to the call winning: a route answering another shape, an OAuth endpoint, says its own; `#[error(quoll::Problem)]` gives a group or a route problems again.
- A `quoll::group!` writes its client's trait before an api lists it, and cannot take the api's: a group of another block says its own `#[error(E)]`, on the group or on each of its routes, and an api with an `#[error(...)]` that lists a group with a route that has none does not compile, its message naming the attribute to write; an api whose `#[error(...)]` is `quoll::Problem`, the default, lists any group.
- `E` implements `Deserialize`, `Display` and `Debug`, and is `Send`, `Sync` and `'static`: the client reads the body of an answer with an error status as the JSON of `E`, whatever its content type, and `E`'s `Display` gives the message, `another api answered 401: Bad credentials`. The author of `E` says where its message is, GitHub's `message`, Stripe's `error.message`, in an `impl Display` or a `#[error("{message}")]` of thiserror. A `Problem` is read from `application/problem+json` alone, and says its `detail`.
- `error.detail()` gives `Option<&E>`, `error.response()` the answer, `Option<&Response<E>>`, its status, its headers and its detail. A body that is not an `E`, an HTML page from a proxy, an empty one, or a 304, gives no detail and no `response()`, and keeps its status and its raw `body()`: `another api answered 502`.
- `?` carries a `quoll::Error<E>` up into a `quoll::Error`, a handler's, through `From`: it keeps its status, its message and its body, and leaves its detail; the handler answers 502, as for any other API's answer.
- A fake builds the error of an answer with `quoll::Error::answered(StatusCode::NOT_FOUND, error)`: its status, its detail, a `response()` with no headers, and no body.
- `#[error(E)]` shapes what the client reads, and nothing else: quoll's server answers problems whatever the contract says, and the OpenAPI document describes them. The items of a stream, `Events` and `JsonLines`, keep `quoll::Error`: a failure after the 200 is quoll's own `event: problem`. The client's constructors, `Api::client` and `client::builder()`, return a `quoll::Result`.

## OpenAPI and TypeScript

- `Api::openapi()` returns the OpenAPI 3.1 document: paths, parameters, bodies, results, schemas from schemars, security schemes, the problem shape of errors. A binary of the contract crate prints it: the schemas are known only once the types are compiled, which a macro does not see.
- It is a `quoll::openapi::Document`: `to_json_pretty()`, `to_json()`, `Serialize`, and `with_title(..)` and `with_version(..)` over its `info`, which is the contract crate's `CARGO_PKG_NAME` and `CARGO_PKG_VERSION`, or the api's name and `0.0.0` for a crate built without Cargo, with the api's `///` as its description.
- An operation is `group_function`, tagged by its group, whose `///` describes the tag. The first paragraph of a route's `///` is its summary, the rest its description.
- Path parameters, query keys and headers are parameters, required unless an `Option` or a `Vec`, never `null`. A fixed header a route sends is a header parameter, not required, whose schema is a string of its value, `const`: not `Accept`, `Content-Type` nor `Authorization`, which OpenAPI describes otherwise, nor a header an argument reads, which the argument describes. A query struct is unfolded into one query parameter per field of its schema, or is one parameter `style: form, explode: true`, not required, when its schema is not a plain object. A joined argument is a parameter `explode: false` of the style of its separator, `form`, `spaceDelimited` or `pipeDelimited`, not required, whose schema is the list's. The key of a query scheme is no parameter: the security describes it.
- Bodies: `application/json`, `application/x-www-form-urlencoded` for `Form<T>`, `text/plain` for `Text`, `application/octet-stream` for `Bytes`; the fixed fields of a body are `const` properties of its schema, required, an `allOf` of the schema of its type and of an object that holds them. Results: 200, 201 with an optional `Location`, 202, 204, and a 204 beside the status of a result whose value may be empty, an `Option<T>`, `()`, `Text`, `Bytes` or a stream, in a `Created` or an `Accepted` too; `text/plain` for `Text`, `*/*` for `Bytes` and for `ByteStream`, whose description says it streams.
- Every error is the `Problem` schema, `application/problem+json`. A route lists what quoll answers itself: 400 when it reads an argument, 401 when it takes a credential, 403 when it needs scopes or takes a cookie scheme the contract declares on a method other than `GET`, `HEAD` and `OPTIONS`, 415 when it reads a body, and `default` for any other.
- Schemes sit under `securitySchemes` by their `NAME`. A route's requirement lists its schemes, any of which serves, each with its scopes, and `{}` when its credential is optional.
- Schemas sit under `components/schemas`, as schemars names them: a second type named `Note` in another module is `Note2`, in the order the contract names them, which `#[schemars(rename = "...")]` changes; a character a component's name does not take, a space, a `<`, is a `_`. A parameter or a body is described as serde reads it, a result or a stream's item as serde writes it; a type whose two schemas differ, as with a `#[serde(default)]` field, has both, `NoteInput` and `NoteOutput`, and so has a type that holds one of them. The two sides are paired by schemars' names: two types of one name, one of them used on one side only, may get misleading `Input` and `Output` names, every `$ref` still pointing at the right schema, which `#[schemars(rename = "...")]` avoids.
- A schema that holds a `$ref` beside other keywords, as schemars writes a newtype variant of an internally tagged enum beside its tag, is an `allOf` of the reference and of the keywords that constrain, with its `description`, `default` and other annotations beside the `allOf`: orval and the generators that follow OpenAPI 3.0 read a `$ref` alone and drop what is beside it.
- A contract of a third-party API that is only called says `#[openapi(none)]` on its api: no `Api::openapi`, and its types, which may come from crates that do not implement `JsonSchema`, need serde alone. A `quoll::group!` of such a contract says it on its group.
- orval writes the TypeScript from it: TanStack Query here; `vue-query`, `svelte-query`, `swr`, `zod` or a plain `fetch` client from the same document.
- The credential goes through orval's mutator, one function of the application; the browser sends cookies itself.
- A stream's items are typed through an extension, as OpenAPI 3.1 cannot type them: the response's media type (`text/event-stream`, `application/jsonl`) carries `x-item`, a `$ref` to the item's schema, which orval writes as a type like any other. The extension goes once the generators read OpenAPI 3.2's item schemas.
- No helper of quoll reads a stream on the web side for now; the browser's own APIs do:
  - **`EventSource`** reads an `Events` route of a same-origin app whose scheme is a cookie, which the browser sends itself. `onmessage` sees unnamed events only: `source.addEventListener("problem", ...)` sees the `event: problem` that ends a stream which failed after the 200, its `data` the problem. After a clean end of the stream, `EventSource` connects again on its own, unless it is closed, and goes on only when that request is answered with a 200: an error, a 5xx of a proxy while the server restarts among them, closes it for good.
  - **`fetch`** and `response.body.getReader()` read a `ByteStream` and a `JsonLines`, and an `Events` route that takes a header credential, which `EventSource` cannot send, with a few lines that split the events. A failure of a `JsonLines` stream aborts its body, an error of the reader, not a clean end.
  - A helper, `subscribe`, comes later, as [Later](#later) says.
- An `i64` is a `number`, exact up to 2^53: ids and counts stay below it. A value that may go beyond it is declared as a `String` in its Rust type.
- Neither the document nor the TypeScript is committed in the example: `cargo quoll gen` writes them, in development and in a build. They are the one generated code on disk, as `tsc` and the editor need files. A project that wants a CI of its JavaScript alone, with no cargo, commits the document and runs `cargo quoll gen --check` in its Rust CI. A test of the example keeps a snapshot of its document, which is no input of the build.

### The dev loop

`cargo quoll`, from the crate `quoll-cli`, writes the documents, runs the commands that follow them, and rebuilds and restarts the servers. It knows no bundler and no generator: the project's own commands, named in the workspace's `Cargo.toml`, do the JavaScript side. It does not link the contract crate: it runs the contract's binary and reads the document from its output, so that its version need not be the contract's version of `quoll`.

```toml
[[workspace.metadata.quoll.api]]
package = "notes-api"
bin = "openapi"                       # the contract's binary, which prints the document
output = "web/openapi.json"
after = [["npx", "--no", "orval"]]    # commands run, in order, after the document

[[workspace.metadata.quoll.server]]
package = "notes-server"
url = "http://127.0.0.1:8080"

[workspace.metadata.quoll.web]
dir = "web"
dev = ["npm", "run", "dev"]           # run by `cargo quoll dev`, beside the server
build = ["npm", "run", "build"]       # run by `cargo quoll build`, after `gen`
```

- **The config**: one `api` per contract, with the package, its binary, the file it writes and its `after`, a list of commands; one `server` per server, with the package and the url it answers on; one `web`, with its directory and its two commands. Paths are relative to the workspace's root. A config that cannot be read is an error that names the key at fault.
- **A command** is a list of words, run with no shell, its program found on the `PATH` as the OS does: `npm` is `npm.cmd` on Windows, resolved, not written per OS. `after` runs in the directory of `output`, `dev` and `build` in `web.dir`.
- **`cargo quoll gen`** builds and runs each api's binary, writes its `output` when its content changed, a file that did not change being left as it is, then runs its `after` commands in order, whether the document changed or not, so that a checkout whose document is committed gets its TypeScript too. It fails when cargo or a command fails, with its output.
- **`cargo quoll gen --check`** writes nothing, and fails when a document on disk is not the one the contract prints: the check of a project that commits its document.
- **`cargo quoll build`** runs `gen`, then `web.build`, and fails when either fails.
- **`cargo quoll dev`** runs `gen`, builds and starts the servers, then watches, each with its tree, the directories of the packages its apis and servers are built from and of their path dependencies outside the workspace, and of theirs, which count as the package of the workspace that depends on them, and the workspace's `Cargo.toml` and `Cargo.lock`, which count as every package. Left out are a package's own `target/`, hidden files and directories and `node_modules` at any depth, the target directory, the documents, and `web.dir`, but the packages inside it, as a Tauri app's `src-tauri`. A watcher that missed events, and asks for a rescan, counts as a change of every package. inotify watches each directory of a tree, `node_modules` and the target directory included, up to `fs.inotify.max_user_watches`: a package at the workspace's root that holds the web side may reach it, which a warning names; the limit is raised, or the web side kept out of that package. The packages, their dependencies and the config are read at the start: a change to them, a package or a path dependency added, is read by the next `cargo quoll dev`, though a change to a manifest makes everything again:
  - **A change to a contract crate**, or to a crate it depends on: its document, then its `after` if the document changed, or if it has not succeeded since the start, then the servers that depend on it. The contract crate holds no axum, so its binary builds in seconds. A document whose content did not change is not rewritten, so that the web side reloads nothing.
  - **A change to a server crate**: that server alone.
  - **A server** runs the binary `cargo run` would: the package's only one, or its `default-run`. It is built with `--message-format=json`, its diagnostics rendered with ANSI colours when stderr is a terminal, and the one running stops only once the new one has compiled. A cargo error is printed with its message, and the last server that compiled keeps running. A build whose executable is the one that runs leaves it running. Its executable is copied under the target directory's `quoll-dev/`, out of cargo's own, before it runs, so that cargo builds the next one meanwhile, which Windows would refuse on an executable that runs, and it finds its dynamic libraries where `cargo run` gives them. A server is ready when its `url`'s host and port accept a connection. One that does not within a minute is stopped and fails, reported as `failed` with the exit `code` of that stop when it has one; one that exits before it answers, or whose port another process holds, fails too, reported as `failed` with its exit `code` when it has one. One that exits on its own is started again by the next change that touches it.
  - **`web.dev`** starts once, after the first round, each server ready or failed, its output passed through, and is not restarted: its bundler watches its own files.
  - **The children**, cargo's builds, the commands, the servers and `web.dev`, each run in a process group of their own on Unix and a job object on Windows, with their stdin closed, so that stopping one stops what it started, the bundler under `npm run dev`. On Unix a child's group is sent `SIGTERM`, then killed once its last process is gone or after 5 s; on Windows its job is terminated. The children of a round a signal cuts short are stopped the same way. Ctrl-C, `SIGTERM` or `SIGHUP` on Unix, Ctrl-C, Ctrl-Break or the console closing on Windows stop every child, `web.dev` first, and a second one kills them at once. A `cargo quoll dev` that is killed takes its children with it on Windows, through their jobs, not on Unix.
  - **`--json`** prints one JSON line per event on stdout, for a tool that reads them, the output for a person going to stderr with the children's:
    - `{"event": "document", "package": "notes-api", "path": "web/openapi.json", "changed": true}`: a document made, written when `changed`, its `path` as `output` writes it.
    - `{"event": "error", "source": "cargo", "package": "notes-server", "message": "mismatched types", "file": "server/src/main.rs", "line": 12, "column": 5}`: an error of the compiler, where it points from the workspace's root, `file`, `line` and `column` null when it points nowhere. `source` is `quoll` for any other failure, a build, a binary or a command that fails, a server that exits before it answers, its `file`, `line` and `column` null, and its `package` null when there is none. Every field of an `error` event is written, null when it has no value; the `code` of a `server` or a `web` event is left out when it has none.
    - `{"event": "server", "state": "ready", "package": "notes-server", "url": "http://127.0.0.1:8080"}`: `state` is `building`, then `ready`, or `failed` when it does not compile or cannot start, the last one that compiled keeping on, with its exit `code` when it exited; `stopped` for a newer build, at the end or on its own, with its exit `code` when it has one.
    - `{"event": "web", "state": "started"}`, then `stopped`, with its exit `code` when it has one.

The JavaScript side follows conventions, with no plugin of quoll for now:

- **The proxy** is the bundler's own, to the server's `url`: Vite's `server.proxy`, Next.js's `rewrites`, with no other configuration. The page calls the api on its own origin, `/api/...`. Vite's proxy holds the head of an answer until its first bytes, which the comment that opens an `Events` answer lets through at once; a `JsonLines` or a `ByteStream` is seen to open with its first line or chunk. A server that `cargo quoll dev` restarts ends its `Events` cleanly where it is sent `SIGTERM` and exits within its grace, on Linux and macOS, which the proxy passes on: the page sees its stream end, and may follow it again once the new server answers. A stream the server cuts rather than ends is not passed on: Vite's proxy keeps the page's side open when the server drops the connection, and the page does not see it end. That is a `JsonLines` or a `ByteStream` at a restart, any stream on Windows, where `cargo quoll dev` terminates the server's job at once, and any stream of a server killed after the 5 s the CLI gives it, or that crashes: a limit of the dev loop with the plain proxy alone, [#70](https://github.com/4thlabs/quoll/issues/70).
- **The generator** keeps its own config, which reads the `output` the api writes, `input: "./openapi.json"` for orval, and `after` runs it. Any generator works, the one the project's command names.
- **A reload** is the bundler's own: its watcher sees the TypeScript the generator rewrites. A cargo error shows in the terminal of `cargo quoll dev`, not in the bundler's overlay.

## A complete example

Notes kept per user: a token for a login, notes to list, read, write and watch, and statistics for operators holding a key. The web page and the command line use bearer tokens, the operators' scripts a key. The contract is one `quoll::api!`, its models a file per domain, as is the server. The sessions and the store, both in memory, are in the repository, `examples/notes/server/src/sessions.rs` and `store.rs`, beside the helpers they share, `utils.rs`, the hashing of passwords included, and are not shown here.

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
//! The contract of the notes example, which the server implements and its clients call.

mod admin;
mod api;
mod auth;
mod notes;

pub use admin::*;
pub use api::*;
pub use auth::*;
pub use notes::*;
```

`api/src/api.rs`

```rust
use quoll::{Created, Events, Form};

use crate::{Login, NewNote, Note, NoteEvent, NoteFilter, Stats, TokenResponse};

quoll::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Form<Login>) -> TokenResponse;
    }

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes"             => search(query filter: NoteFilter) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        GET    "/notes/{id}/events" => events(id: i64) -> Events<NoteEvent>;
    }

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> Stats;
    }
}
```

`api/src/auth.rs`

```rust
use std::fmt;

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Login {
    pub user: String,
    pub password: String,
}

/// Never shows the password.
impl fmt::Debug for Login {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Login")
            .field("user", &self.user)
            .finish_non_exhaustive()
    }
}

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct TokenResponse {
    /// Sent as `Authorization: Bearer <token>` to the routes of the notes.
    pub access_token: String,
}

/// Never shows the token.
impl fmt::Debug for TokenResponse {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TokenResponse").finish_non_exhaustive()
    }
}
```

`api/src/notes.rs`

```rust
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
    pub body: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
    pub body: String,
}

pub const CONTAINS_LIMIT: usize = 1000;

/// Each key given, and not empty, narrows the search.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    /// Notes whose title or text holds this, whatever the case of its ASCII letters: 1000 characters at most.
    #[schemars(length(max = CONTAINS_LIMIT))]
    pub contains: Option<String>,
    /// Notes tagged so, by a `#word` of their title or their text: `#milk` or `milk,` is `milk`.
    pub tag: Option<String>,
    /// The page, of 20 notes in the order they were written, from 1, the first when it is missing.
    pub page: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum NoteEvent {
    Edited(Note),
    /// The last event of the note's stream.
    Removed {
        id: i64,
    },
}
```

`api/src/admin.rs`

```rust
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Stats {
    pub users: u64,
    pub notes: u64,
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
//! The server of the notes example, its store in memory.

mod admin;
mod app;
mod auth;
mod notes;
mod sessions;
mod store;
mod utils;

pub use app::{App, router, server};
pub use auth::{Operator, User};
pub use sessions::{SESSION_LIFETIME, Sessions};
pub use store::{PAGE_SIZE, Store, StoreError};
pub use utils::random_hex;
```

`server/src/app.rs`

```rust
use std::time::Duration;

use notes_api::{Api, ApiServices};
use quoll::server;

use crate::{Sessions, Store};

#[derive(Debug, Clone)]
pub struct App {
    pub store: Store,
    pub sessions: Sessions,
}

impl App {
    pub fn in_memory() -> Self {
        Self {
            store: Store::in_memory(),
            sessions: Sessions::default(),
        }
    }
}

pub fn router(app: App) -> axum::Router {
    Api::router(ApiServices {
        auth: app.clone(),
        notes: app.clone(),
        admin: app,
    })
}

/// The layers go in any order: a request gets its id, then its span, then its deadline and its body's limit.
pub fn server() -> server::Builder {
    server::builder()
        .with(server::request_id())
        .with(server::trace())
        .with(server::timeout(Duration::from_secs(10)))
        .with(server::body_limit(1 << 20))
}
```

`server/src/auth.rs`

```rust
use notes_api::{AuthServer, Key, Login, TokenResponse};
use quoll::{Authenticate, Bearer, Error, Form, Response, Result};

use crate::App;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct User {
    pub id: i64,
    pub name: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Operator {
    pub name: String,
}

impl AuthServer for App {
    async fn token(&self, login: Form<Login>) -> Result<Response<TokenResponse>> {
        let user = self
            .store
            .check_password(&login.user, &login.password)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::unauthorized("wrong user or password"))?;

        Ok(Response::new(TokenResponse {
            access_token: self.sessions.open(user),
        }))
    }
}

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        self.sessions
            .user(&token)
            .ok_or_else(|| Error::unauthorized("unknown or expired token"))
    }
}

impl Authenticate<Key> for App {
    type Caller = Operator;

    async fn authenticate(&self, key: String) -> Result<Operator> {
        self.store
            .operator(&key)
            .ok_or_else(|| Error::unauthorized("unknown key"))
    }
}
```

`server/src/notes.rs`

```rust
use axum::http::header::LOCATION;
use notes_api::{CONTAINS_LIMIT, NewNote, Note, NoteEvent, NoteFilter, NotesServer};
use quoll::{Error, Events, Response, Result};

use crate::{App, User};

impl NotesServer for App {
    async fn search(&self, user: &User, filter: NoteFilter) -> Result<Response<Vec<Note>>> {
        // The document says 1000 at most: a caller who skips its check meets the same rule, and a search under the
        // store's lock stays short.
        if filter
            .contains
            .as_ref()
            .is_some_and(|text| text.chars().count() > CONTAINS_LIMIT)
        {
            return Err(Error::bad_request(format!(
                "`contains` holds more than {CONTAINS_LIMIT} characters"
            )));
        }

        Ok(Response::new(self.store.notes(user.id, &filter)))
    }

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

    async fn update(&self, user: &User, id: i64, note: NewNote) -> Result<Response<Note>> {
        self.store
            .replace(user.id, id, note)
            .map(Response::new)
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn remove(&self, user: &User, id: i64) -> Result<Response<()>> {
        self.store.remove(user.id, id);
        Ok(Response::new(()))
    }

    async fn events(&self, user: &User, id: i64) -> Result<Response<Events<NoteEvent>>> {
        let changes = self
            .store
            .watch(user.id, id)
            .ok_or_else(|| Error::not_found("no such note"))?;

        Ok(Response::new(Events::new(changes)))
    }
}
```

`server/src/admin.rs`

```rust
use notes_api::{AdminServer, Stats};
use quoll::{Response, Result};

use crate::{App, Operator};

impl AdminServer for App {
    async fn stats(&self, _operator: &Operator) -> Result<Response<Stats>> {
        Ok(Response::new(self.store.stats()))
    }
}
```

`server/src/main.rs`

```rust
#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let app = notes_server::App::in_memory();
    let password = notes_server::random_hex();

    app.store.add_user("demo", &password).await?;

    let key = app.store.add_operator("ops")?;

    println!("throwaway credentials of a demo, made anew at each start:");
    println!("sign in as demo, password {password}, at POST /api/token");
    println!("an operator's X-Api-Key: {key}");

    // Until Ctrl-C or SIGTERM, then up to 5 s for the requests it serves.
    notes_server::server()
        .serve("0.0.0.0:8080", notes_server::router(app))
        .await?;
    Ok(())
}
```

`server/tests/notes.rs`

```rust
use notes_api::{Api, NewNote, Notes};
use notes_server::User;
use quoll::client;

fn served(app: notes_server::App) -> axum::Router {
    notes_server::server().into_router(notes_server::router(app))
}

#[tokio::test]
async fn a_created_note_reads_back() -> anyhow::Result<()> {
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let url = quoll::test::spawn(served(app)).await;
    let api = client::builder()
        .with(client::bearer(token))
        .build::<Api>(&url)?;

    let created = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
            body: String::new(),
        })
        .await?;
    let read = api.notes().get(created.id).await?;

    assert_eq!(read.title, "Milk");
    Ok(())
}

#[tokio::test]
async fn a_call_without_token_is_refused() -> anyhow::Result<()> {
    let url = quoll::test::spawn(served(notes_server::App::in_memory())).await;
    let api = Api::client(&url)?;

    let error = api.notes().get(1).await.unwrap_err();

    assert_eq!(error.status(), Some(reqwest::StatusCode::UNAUTHORIZED));
    Ok(())
}
```

`cli/src/main.rs`

```rust
use futures::StreamExt;
use notes_api::{Api, Auth, Login, NewNote, NoteFilter, Notes};
use quoll::{Form, client};
use reqwest::StatusCode;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let url = std::env::var("NOTES_URL")?;
    let login = Login {
        user: std::env::var("NOTES_USER")?,
        password: std::env::var("NOTES_PASSWORD")?,
    };
    let token = Api::client(&url)?.auth().token(Form(login)).await?;

    let api = client::builder()
        .with(client::user_agent("notes-cli/0.1"))
        .with(client::request_id())
        .with(client::retry(3))
        .with(client::bearer(token.access_token))
        .build::<Api>(&url)?;

    let created = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
            body: String::new(),
        })
        .await?;
    let filter = NoteFilter {
        contains: Some("milk".into()),
        ..Default::default()
    };

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

`Cargo.toml`, the workspace's, a workspace of its own as an application's is, in the repository's `examples/notes/`: `cargo quoll dev` in its directory runs the server and Vite, `cargo quoll build` writes the TypeScript then bundles the page, in CI too, so that a change of the contract that breaks the page breaks the pull request.

```toml
[workspace]
members = ["api", "server", "cli"]
resolver = "3"

[[workspace.metadata.quoll.api]]
package = "notes-api"
bin = "openapi"
output = "web/openapi.json"
after = [["npx", "--no", "orval"]]

[[workspace.metadata.quoll.server]]
package = "notes-server"
url = "http://127.0.0.1:8080"

[workspace.metadata.quoll.web]
dir = "web"
dev = ["npm", "run", "dev"]
build = ["npm", "run", "build"]
```

`web/vite.config.ts`

```typescript
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      // The server's `url` in the workspace's Cargo.toml, `[[workspace.metadata.quoll.server]]`: the two change together.
      "/api": "http://127.0.0.1:8080",
    },
  },
});
```

`web/orval.config.ts`: one file, whose hooks' data is the body the mutator returns.

```typescript
import { defineConfig } from "orval";

export default defineConfig({
  notes: {
    input: "./openapi.json",
    output: {
      target: "./src/api/generated.ts",
      client: "react-query",
      httpClient: "fetch",
      override: {
        mutator: { path: "./src/api/fetch.ts", name: "apiFetch" },
        // The hooks' data is the body, which the mutator returns, not orval's `{ data, status, headers }`.
        fetch: { includeHttpResponseReturnType: false },
      },
    },
  },
});
```

The page takes a bearer token, which `EventSource` cannot send: `apiEvents` reads the events with `fetch`, and splits them itself, as quoll's server writes them, each line ending in `\n`. Every event that has data is an item, whatever its name, an event with no data and a comment are skipped, and an `event: problem` ends the stream with an error; `onOpen` hears of its opening, after which the page reads the note again, and `onEnd` of an end the page did not ask for, a failure or a clean end. An error the server answers is an `ApiError`, whose message is the problem's detail, which the page shows; a refusal of the token the page holds signs it out.

`web/src/api/fetch.ts`

```typescript
import type { Problem } from "./generated";

let token: string | undefined;

export function signIn(accessToken: string) {
  token = accessToken;
}

export function signOut() {
  token = undefined;
}

// An error the server answered, its message the problem's detail: what the page shows. `expired` when the server
// refused the token that the page still holds: it expired, or the server that gave it restarted.
export class ApiError extends Error {
  readonly status: number;
  readonly problem: Problem | undefined;
  readonly expired: boolean;

  constructor(status: number, problem?: Problem, expired = false) {
    super(problem?.detail ?? problem?.title ?? `the server answered ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.problem = problem;
    this.expired = expired;
  }
}

// The error type of orval's hooks, whatever the problem a route declares: every one is a `Problem`.
export type ErrorType<_Body> = ApiError;

function authorization(init?: HeadersInit): Headers {
  const headers = new Headers(init);

  if (token) {
    headers.set("Authorization", `Bearer ${token}`);
  }

  return headers;
}

export function isExpired(error: unknown): boolean {
  return error instanceof ApiError && error.expired;
}

// The error of a request sent with the token `sent`. A refusal of a token signed out since, answered late, is no news
// of the session the page holds now.
async function apiError(response: Response, sent: string | undefined): Promise<ApiError> {
  const expired = response.status === 401 && sent !== undefined && sent === token;

  return new ApiError(response.status, await problemOf(response), expired);
}

// A proxy that cannot reach the server answers with no problem, and a body may be cut: the status alone is the error.
async function problemOf(response: Response): Promise<Problem | undefined> {
  if (!response.headers.get("Content-Type")?.startsWith("application/problem+json")) {
    return undefined;
  }

  try {
    return await response.json();
  } catch {
    return undefined;
  }
}

export async function apiFetch<T>(url: string, init: RequestInit): Promise<T> {
  const sent = token;
  const response = await fetch(url, { ...init, headers: authorization(init.headers) });

  if (!response.ok) {
    throw await apiError(response, sent);
  }

  // A 204 or a 205 has no body: `null`, the JSON of an `Option` that is `None`.
  const empty = response.status === 204 || response.status === 205;

  return (empty ? null : await response.json()) as T;
}

export type EventHandlers<T> = {
  // Each event's data, parsed.
  onItem: (item: T) => void;
  // The stream is open: what changed before it is not in it.
  onOpen?: () => void;
  // An end that was not asked for: the error of a failure, before the stream or within it, or nothing when the
  // server ended the stream.
  onEnd?: (error?: unknown) => void;
};

// Reads a stream of server-sent events until the function it returns is called.
export function apiEvents<T>(
  url: string,
  { onItem, onOpen = () => {}, onEnd = logFailure }: EventHandlers<T>,
): () => void {
  const controller = new AbortController();

  function ended(error?: unknown) {
    if (!controller.signal.aborted) {
      onEnd(error);
    }
  }

  readEvents(url, onItem, onOpen, controller.signal).then(() => ended(), ended);

  return () => controller.abort();
}

function logFailure(error?: unknown) {
  if (error !== undefined) {
    console.error(error);
  }
}

async function readEvents<T>(url: string, onItem: (item: T) => void, onOpen: () => void, signal: AbortSignal) {
  const sent = token;
  const response = await fetch(url, { headers: authorization(), signal });

  if (!response.ok) {
    throw await apiError(response, sent);
  }

  // A 204 or a 205 has no body: a stream that ends at once.
  if (response.body === null) {
    return;
  }

  onOpen();

  const reader = response.body.pipeThrough(new TextDecoderStream()).getReader();
  let buffer = "";

  for (;;) {
    const { done, value } = await reader.read();

    if (done) {
      return;
    }

    // A chunk may end within an event: its start waits in `buffer` for the next one.
    const blocks = (buffer + value).split("\n\n");
    buffer = blocks.pop() ?? "";

    for (const block of blocks) {
      const { event, data } = parseEvent(block);

      if (data === undefined) {
        continue;
      }

      // The stream failed after its 200: the problem says the status it would have answered.
      if (event === "problem") {
        const problem: Problem = JSON.parse(data);

        throw new ApiError(problem.status ?? 500, problem);
      }

      onItem(JSON.parse(data) as T);
    }
  }
}

// One event, its lines `field: value`; a line that starts with `:` is a comment.
function parseEvent(block: string): { event?: string; data?: string } {
  let event: string | undefined;
  let data: string | undefined;

  for (const line of block.split("\n")) {
    const colon = line.indexOf(":");
    const field = colon < 0 ? line : line.slice(0, colon);
    const value = colon < 0 ? "" : line.slice(colon + 1).replace(/^ /, "");

    if (field === "event") {
      event = value;
    } else if (field === "data") {
      data = data === undefined ? value : `${data}\n${value}`;
    }
  }

  return { event, data };
}
```

`web/src/notes/NoteView.tsx`, a note, edited in a `NoteEditor` beside it, read again when it changes elsewhere, and saying when its changes no longer arrive.

```typescript
import { useQueryClient } from "@tanstack/react-query";
import { useEffect, useEffectEvent, useState } from "react";

import { apiEvents, isExpired } from "../api/fetch";
import {
  getNotesEventsUrl,
  getNotesGetQueryKey,
  getNotesSearchQueryKey,
  type NewNote,
  type Note,
  type NoteEvent,
  useNotesGet,
  useNotesRemove,
  useNotesUpdate,
} from "../api/generated";
import { ErrorMessage } from "../ErrorMessage";
import { NoteEditor } from "./NoteEditor";

type Props = {
  id: number;
  onClosed: () => void;
  onExpired: () => void;
};

export function NoteView({ id, onClosed, onExpired }: Props) {
  const queryClient = useQueryClient();

  function listChanged() {
    queryClient.invalidateQueries({ queryKey: getNotesSearchQueryKey() });
  }

  const { data: note, error, refetch } = useNotesGet(id);
  // The cache follows a change here, as `mutate`'s own callbacks do not once the view is gone. The note is read
  // again rather than set to the answer, which a change elsewhere, read meanwhile, may be newer than.
  const update = useNotesUpdate({
    mutation: {
      onSuccess: () => {
        queryClient.invalidateQueries({ queryKey: getNotesGetQueryKey(id) });
        listChanged();
      },
    },
  });
  const remove = useNotesRemove({ mutation: { onSuccess: listChanged } });
  const [editing, setEditing] = useState(false);
  // Whether the note's events still arrive; following them again starts a new stream.
  const [live, setLive] = useState(true);
  const [follows, setFollows] = useState(0);

  // Another tab or the command line changed the note: read it again, or close it once it is removed.
  const changed = useEffectEvent((event: NoteEvent) => {
    listChanged();

    switch (event.kind) {
      case "edited":
        refetch();
        break;
      case "removed":
        onClosed();
        break;
    }
  });

  // A change made before the stream opened is not in it: the note is read again once it is open.
  const opened = useEffectEvent(() => refetch());

  // The stream ended before the note was removed: the server stopped, or the network failed.
  const ended = useEffectEvent((error?: unknown) => {
    if (isExpired(error)) {
      onExpired();
    } else {
      setLive(false);
    }
  });

  useEffect(() => {
    return apiEvents<NoteEvent>(getNotesEventsUrl(id), { onItem: changed, onOpen: opened, onEnd: ended });
  }, [id, follows]);

  if (!note) {
    return error ? <ErrorMessage error={error} /> : <p>Loading</p>;
  }

  function follow() {
    setLive(true);
    setFollows(follows + 1);
  }

  function save(changes: NewNote) {
    update.mutate({ id, data: changes }, { onSuccess: () => setEditing(false) });
  }

  function removeNote() {
    remove.mutate({ id }, { onSuccess: onClosed });
  }

  return (
    <>
      <ErrorMessage error={error} />
      {!live && (
        <p role="status">
          Changes made elsewhere no longer show. <button onClick={follow}>Follow them again</button>
        </p>
      )}
      {editing ? (
        <NoteEditor
          note={note}
          saving={update.isPending}
          error={update.error}
          onSave={save}
          onCancel={() => setEditing(false)}
        />
      ) : (
        <NoteText note={note} error={remove.error} onEdit={() => setEditing(true)} onRemove={removeNote} />
      )}
    </>
  );
}

type NoteTextProps = {
  note: Note;
  error: Error | null;
  onEdit: () => void;
  onRemove: () => void;
};

function NoteText({ note, error, onEdit, onRemove }: NoteTextProps) {
  return (
    <article className="note">
      <h2>{note.title}</h2>
      <p className="body">{note.body}</p>
      <div className="actions">
        <button onClick={onEdit}>Edit</button>
        <button onClick={onRemove}>Remove</button>
      </div>
      <ErrorMessage error={error} />
    </article>
  );
}
```

## What does not compile

- A `{name}` with no argument of that name; an argument that is neither in the string, a header, the query struct nor the one body.
- Two bodies; a body on `GET`, `HEAD` or `OPTIONS`; a stream, `Events`, `JsonLines` or `ByteStream`, as a body; a result on `HEAD`, whose answer has no body.
- A path parameter of a type the macro sees is not read from text: a `Vec` or another collection, a map, an `Option`, a reference, a tuple, or `Form`, `Text`, `Bytes`, `Created` or a stream; a header of such a type, but an `Option` of a type read from text. A struct of the application cannot be told from an enum of unit variants, and is refused when it is called: the server answers 500, logged, and the client a misuse error, before sending.
- A literal segment or a literal key of the query of other characters than ASCII letters, digits and `-._~`, or `.` or `..`; a key whose brackets are not closed, `filter[name`, come first, `[name]`, are followed by text, `a[b]c`, or hold a bracket, `.` or `..`.
- A query struct and a query in the string together; two query structs; a query struct whose type is not a struct: a scalar, a `String`, an `Option`, a `Vec`...
- `#[join(...)]` with a separator other than `","`, `" "` and `"|"`, twice, on an argument that is not a `key={name}` of the query, or on an argument that is not a `Vec` of a type read from text; another attribute on an argument.
- A header read by two arguments; an argument that is a header or the query struct and a `{name}` of the string too.
- A header that quoll or HTTP writes itself, `Content-Type`, `Content-Length`, `Host`, `Transfer-Encoding` and the hop-by-hop ones, and `Authorization` on a route that takes a scheme.
- A `Created<T>` or an `Accepted<T>` of a type of quoll's, `Created<Text>`, `Bytes`, a stream or a `Form`: its `T` is the JSON it answers.
- In `#[header(...)]`: anything but `"name" = value`, `"name" = none` or `none`; a name that is not a token, or a header that quoll or HTTP writes but `Content-Type`; a value that is not visible ASCII with blanks inside it only, a string or a `&str` constant's; a header named twice by one item, `none` twice, or a header left beside `#[header(none)]`; a `none` on the api, in the api's own block on a header the parent does not fix, or, in any block, a route's on a header its group already leaves; a `Content-Type` of another media type than the body's or of another charset than UTF-8, a route's own on a route with no body, or any on a `Bytes` body; a header that carries the credential of a scheme the route takes; `#[header(...)]` on a scheme. What a group of another block takes of its api's is checked by the api, its message on the group in the list; what it leaves is not.
- One method and path twice in an api, whichever blocks its groups come from, unless a field both routes fix in their `#[body(...)]` has two values of one kind; two such routes in an api with a document, or in a `quoll::group!` with one.
- In `#[body(...)]`: anything but `"name" = value`, a value that is not a bool, a number of an `i64` or a `u64` or a finite float, or a string; a field named twice by one route; `#[body(...)]` on the api, a group or a scheme, on a route with no body, or with a `Text` or a `Bytes` body.
- A group less visible than its api, whose router and client name the group's items.
- A group listed twice, or of the name of another group of the api; an empty list; a path of the list that is not a group, or a group named alone that is not in scope; a group of the list with `#[openapi(none)]` in an api with a document. A `quoll::group!` with no group, two groups, an api or a scheme.
- Two items the macro writes with one name: a group named as its api, or as the `NotesServer`, `NotesClient`, `ApiServices` or `ApiClient` the contract writes for another; two routes of one operation id, as `tags_list` in a group `Note` and `list` in a group `NoteTags`, both `note_tags_list`.
- A route left out of a service or a fake, a handler or a fake's method of the wrong signature: Rust's own errors on the trait.
- `client::builder().build::<T>(url)` for a type that is not the api of a contract: the message says what an api is.
- A group without a service; a service of a group with authentication that does not implement `Authenticate` for its schemes; two schemes of one `#[auth(A | B)]` giving callers of two types; scopes on a caller that does not implement `Scoped`.
- A scheme in `#[auth(...)]` that is neither given by the crate, declared in the contract nor a type implementing `Scheme`; a scheme's credential that is not `Send`; `key::<K>` for a scheme the contract does not declare; two schemes of one `NAME` in an api's block, which names a scheme in the OpenAPI document (from two blocks, `Api::openapi()` panics), or a `NAME` of other characters than ASCII letters, digits, `.`, `-` and `_`.
- A type of a contract that does not implement `JsonSchema`, unless the api or the `quoll::group!` says `#[openapi(none)]`; `#[openapi(...)]` with anything but `none`, or on a scheme, a route or a group of the api's block.
- In `#[auth(...)]`: `none` beside a scheme or an option; a scheme twice in an alternative, or more than four; in the api's block, a path to a declared scheme, `self::Key`, and anywhere, a path through `quoll` to the crate's, `quoll::Bearer`, which are named alone; two schemes of an alternative of one `NAME`, as `Key | crate::Key` in a `quoll::group!`; two schemes of an alternative that read one place, `Bearer | Login` for an `openid` scheme or two header schemes of one header; `optional` or `scopes` twice, or an option it does not take; `scopes()` empty, a scope written twice or that is not an OAuth `scope-token`; `optional` with `scopes`.
- A declared scheme of a kind other than `header`, `cookie`, `query` and `openid`; named `Bearer`, `Basic` or `none`, with a name that is not ASCII, or with the name of another scheme or of an item the macro writes; a header that quoll or HTTP writes, `Authorization` or `Cookie`; a header or a cookie whose name is not a token, a query key other than letters, digits and `-._~`; an `openid` URL that is not `http(s)://`; a scheme less visible than a group that takes it; `#[auth(...)]` on a scheme.
- An argument that reads the credential of a scheme its route takes: the header of a header scheme, `Cookie` beside a cookie scheme, the query key of a query scheme.

Each message the macro writes points at the token at fault and says what it expected, and has a test (trybuild).

## Version A, the other spelling

Attributes on a trait. Not chosen, but kept possible: both spellings are read into one model of the api, from which one generator writes the code, so A can be added later, or groups of both sit in one api, as an api lists its groups through their descriptors. Neither spelling says anything the other cannot.

```rust
#[quoll::group(auth = Bearer)]
pub trait Notes {
    #[get("/notes/{id}")]
    async fn get(&self, id: i64) -> Result<Note>;

    #[post("/notes")]
    async fn create(&self, note: NewNote) -> Result<Created<Note>>;
}

#[quoll::api(prefix = "/api", groups(Notes))]
pub struct Api;
```

rustfmt formats it and rust-analyzer reads it as any Rust; it repeats `&self`, `async` and `Result` on every route. B reads as a table of routes, and the compiler checks both the same way.

## Later

Decided, not in the first version. The first two change no contract already written.

1. **Typed errors per route** of quoll's server, as the Errors section says.
2. **Pagination by header.** GitHub pages with a `Link` header (`rel="next"`, `rel="last"`), which a result cannot say yet: a result `Paged<T>` carrying it, and on the client `.pages()`, a stream that follows the links.
3. **A `quoll::PathValue` derive** on unit enums and newtypes, so that a path parameter not read from text is refused at compile time: a struct of the application in a path no longer compiles.
4. **A `subscribe` helper** for the web side, in an npm package of its own: `subscribe<T>(url, onItem, init)` reads a stream with `fetch`, so that it sends a header credential, calls `onItem` with each item parsed and returns the function that stops it. It reads `Events`, every event that has data an item whatever its name and `event: problem` an error, and `JsonLines`, an aborted body an error, not a clean end, and does not connect again after a clean end; `ByteStream` too, with `onChunk`.
5. **Two routes of one method and path in the OpenAPI document**, told apart by their `#[body(...)]`: one operation whose body is a `oneOf` of their bodies with a `discriminator` on the field that tells them apart, so that an api with a document takes them.
