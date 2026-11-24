//! A modern, type-safe REST toolkit for Rust: from a client for any API to the server behind your frontend.
//!
//! One contract gives you an axum server, a reqwest client and an OpenAPI 3.1 document. The contract is a route table,
//! written with [`api!`]. The server implements a trait per group, the client calls a method per route, and
//! `Api::openapi()` gives the document, which any OpenAPI generator reads to write a frontend's client. A mistake on either side, a
//! route without its handler, a call with the wrong arguments, does not compile.
//!
//! An API that is only called, a third-party one, is a contract too: its api says `#[openapi(none)]`, and it is a
//! typed client alone, whose types need serde and no schema: see [OpenAPI](#openapi).
//!
//! From [Routes](#routes) on, this page is a map of what a contract holds, a form at a time. The syntax as a whole,
//! and the items the macro generates, are in the doc of [`api!`]; a contract split across modules is in [`group!`].
//!
//! # A contract
//!
//! ```
//! use quoll::Created;
//! use schemars::JsonSchema;
//! use serde::{Deserialize, Serialize};
//!
//! #[derive(Serialize, Deserialize, JsonSchema)]
//! pub struct Note {
//!     pub id: i64,
//!     pub title: String,
//! }
//!
//! #[derive(Serialize, Deserialize, JsonSchema)]
//! pub struct NewNote {
//!     pub title: String,
//! }
//!
//! quoll::api! {
//!     /// Notes, kept per user.
//!     pub api Api = "/api";
//!
//!     /// The signed-in user's notes.
//!     #[auth(Bearer)]
//!     pub group Notes {
//!         GET    "/notes/{id}" => get(id: i64) -> Note;
//!         POST   "/notes"      => create(note: NewNote) -> Created<Note>;
//!         DELETE "/notes/{id}" => remove(id: i64);
//!     }
//! }
//!
//! let document = Api::openapi().to_json_pretty();
//!
//! assert!(document.contains(r#""operationId": "notes_get""#));
//! ```
//!
//! # Its server
//!
//! With the feature `server`, the group is a trait, `NotesServer`, whose handlers receive the caller that
//! [`Authenticate`] makes of the request's credential. `Api::router` gives the `axum::Router`,
//! which [`server::builder()`] serves with its layers:
//!
#![cfg_attr(feature = "server", doc = "```no_run")]
#![cfg_attr(not(feature = "server"), doc = "```ignore")]
//! # use quoll::Created;
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct Note { pub id: i64, pub title: String }
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct NewNote { pub title: String }
//! # quoll::api! {
//! #     pub api Api = "/api";
//! #     #[auth(Bearer)]
//! #     pub group Notes {
//! #         GET    "/notes/{id}" => get(id: i64) -> Note;
//! #         POST   "/notes"      => create(note: NewNote) -> Created<Note>;
//! #         DELETE "/notes/{id}" => remove(id: i64);
//! #     }
//! # }
//! use http::header::LOCATION;
//! use quoll::{Authenticate, Bearer, Error, Response, Result, server};
//!
//! struct App;
//!
//! /// Who a request comes from, once its token is checked.
//! struct User {
//!     id: i64,
//! }
//!
//! impl Authenticate<Bearer> for App {
//!     type Caller = User;
//!
//!     async fn authenticate(&self, token: String) -> Result<User> {
//!         match token.as_str() {
//!             "mF_9.B5f-4.1JqM" => Ok(User { id: 1 }),
//!             _ => Err(Error::unauthorized("unknown or expired token")),
//!         }
//!     }
//! }
//!
//! impl NotesServer for App {
//!     async fn get(&self, user: &User, id: i64) -> Result<Response<Note>> {
//!         if id != user.id {
//!             return Err(Error::not_found("no such note"));
//!         }
//!
//!         Ok(Response::new(Note { id, title: "Milk".into() }))
//!     }
//!
//!     async fn create(&self, _user: &User, note: NewNote) -> Result<Response<Note>> {
//!         let note = Note { id: 2, title: note.title };
//!         let location = format!("/api/notes/{}", note.id);
//!
//!         Ok(Response::new(note).header(LOCATION, location))
//!     }
//!
//!     async fn remove(&self, _user: &User, _id: i64) -> Result<Response<()>> {
//!         Ok(Response::new(()))
//!     }
//! }
//!
//! #[tokio::main]
//! async fn main() -> std::io::Result<()> {
//!     server::builder()
//!         .with(server::request_id())
//!         .with(server::trace())
//!         .serve("0.0.0.0:8080", Api::router(ApiServices { notes: App }))
//!         .await
//! }
//! ```
//!
//! # Its client
//!
//! With the feature `client`, the group is a trait, `Notes`, which the client implements, and a test's fake too.
//! [`client::builder()`] builds the client with its middleware:
//!
#![cfg_attr(feature = "client", doc = "```no_run")]
#![cfg_attr(not(feature = "client"), doc = "```ignore")]
//! # use quoll::Created;
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct Note { pub id: i64, pub title: String }
//! # #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
//! # pub struct NewNote { pub title: String }
//! # quoll::api! {
//! #     pub api Api = "/api";
//! #     #[auth(Bearer)]
//! #     pub group Notes {
//! #         GET    "/notes/{id}" => get(id: i64) -> Note;
//! #         POST   "/notes"      => create(note: NewNote) -> Created<Note>;
//! #         DELETE "/notes/{id}" => remove(id: i64);
//! #     }
//! # }
//! use quoll::client;
//!
//! #[tokio::main]
//! async fn main() -> quoll::Result<()> {
//!     let api = client::builder()
//!         .with(client::bearer("mF_9.B5f-4.1JqM"))
//!         .with(client::retry(3))
//!         .build::<Api>("http://127.0.0.1:8080")?;
//!
//!     let note = api.notes().get(1).await?;
//!
//!     println!("{}", note.title);
//!     Ok(())
//! }
//! ```
//!
//! An argument the contract writes with a `&` is borrowed by the client, and received owned by the server: `&str` is a
//! `String` there, `&[T]` a `Vec<T>` and any other `&X` an `X`: `list(owner: &str)` is called as `list("rust-lang")`,
//! with no `.into()`. An argument with no `&` is taken by value.
//!
//! # Routes
//!
//! An api is a route table: the method, the path below the api's prefix, then the function the route becomes. The
//! methods are `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD` and `OPTIONS`. The prefix has no parameter, and `""`
//! is none. Routes sit in groups, and each route's operation id is its group and its function, `notes_get`.
//!
//! The `///` comments of the api, of its groups and of its routes become the docs of the items the macro generates,
//! and the descriptions of the OpenAPI document.
//!
//! ```
//! quoll::api! {
//!     /// The notes of a team.
//!     pub api Api = "/api";
//!
//!     /// The notes.
//!     pub group Notes {
//!         /// Every note's title.
//!         GET    "/notes"      => list() -> Vec<String>;
//!         HEAD   "/notes/{id}" => exists(id: i64);
//!         PUT    "/notes/{id}" => write(id: i64, title: String);
//!         DELETE "/notes/{id}" => remove(id: i64);
//!     }
//! }
//! ```
//!
//! # Parameters
//!
//! Each argument of a route is matched to its string by its name, not by its position:
//!
//! - a `{name}` of the path is a path parameter, read from its text as axum's `Path` reads it: an integer, a string,
//!   a UUID, a unit variant;
//! - a `key={name}` of the query is a query parameter;
//! - `query filter: NoteFilter` is a query struct, the whole query;
//! - `header("If-Match") version: Option<String>` is a header;
//! - the one argument left is the body, on `POST`, `PUT`, `PATCH` and `DELETE` only.
//!
//! ```
//! use quoll::{Bytes, Form, Text};
//! use schemars::JsonSchema;
//! use serde::{Deserialize, Serialize};
//!
//! #[derive(Default, Serialize, Deserialize, JsonSchema)]
//! pub struct NoteFilter {
//!     pub contains: Option<String>,
//!     #[serde(default)]
//!     pub tags: Vec<String>,
//! }
//!
//! #[derive(Serialize, Deserialize, JsonSchema)]
//! pub struct Login {
//!     pub user: String,
//!     pub password: String,
//! }
//!
//! quoll::api! {
//!     pub api Api = "/api";
//!
//!     pub group Notes {
//!         GET  "/notes/{id}"                   => get(id: i64) -> String;
//!         GET  "/notes?page={page}&tag={tags}" => list(page: Option<u32>, tags: Vec<String>) -> Vec<String>;
//!         GET  "/search"                       => search(query filter: NoteFilter) -> Vec<String>;
//!         POST "/notes"                        => create(title: String) -> i64;
//!         POST "/token"                        => token(login: Form<Login>) -> String;
//!         PUT  "/notes/{id}/file"              => upload(id: i64, file: Bytes);
//!         PUT  "/notes/{id}/text"
//!              => write(id: i64, header("If-Match") version: Option<String>, text: Text);
//!     }
//! }
//! ```
//!
//! A path parameter whose type is a struct of the application is refused when it is called: the server answers 500,
//! logged, and the client a misuse error, before sending.
//!
//! The query of a route is read as serde_html_form reads a form, a field per key. An `Option` may be missing; an
//! empty value, `?page=`, is `None` for a number or a `bool` and `Some("")` for a string. A `Vec` takes every value of
//! its key, and is empty when the key is missing. The client writes it so: a `None` leaves its key out, and a `Vec`
//! repeats it.
//!
//! A key of the route's string is ASCII letters, digits and `-._~`, and may end in brackets that hold the same or
//! nothing, as other APIs name their keys: `"/customers/{id}?expand[]={expand}"`,
//! `"/articles?filter[name]={name}&page[size]={size}"`. The client escapes them, `expand%5B%5D=sources`, as a
//! browser's form does, and the server reads them escaped or as they are.
//!
//! A query struct is the whole query: its fields are the keys, as serde names them, read and written the same way. It
//! does not go with a query in the route's string. A `Vec` field that may be missing takes `#[serde(default)]`, as
//! serde_html_form asks. The struct needs no `Default`: an empty query reads when each of its keys may be missing.
//! `Default` is for the callers, which fill the keys they need, `NoteFilter { contains: Some("milk".into()),
//! ..Default::default() }`.
//!
//! `#[join(sep)]` on an argument writes a `Vec` as the one value of its key: `#[join(",")]` is `labels=bug,docs`,
//! `#[join(" ")]` `labels=bug%20docs`, `#[join("|")]` `labels=bug|docs`, and the document says the style. The values
//! are read from text, as a path parameter is; an empty list leaves the key out, and `labels=` reads as one. The server
//! decodes the value before it splits it, so that it reads `labels=bug%2Cdocs` too: a value that holds the separator
//! is refused by the client, before sending. Only an argument is joined: a `Vec` field of a query struct repeats its
//! key.
//!
//! ```
//! quoll::api! {
//!     pub api Api = "/api";
//!
//!     pub group Issues {
//!         GET "/issues?labels={labels}" => list(#[join(",")] labels: Vec<String>) -> Vec<String>;
//!     }
//! }
//! ```
//!
//! A header is read from its text as a path parameter is. An `Option` is `None` when the header is missing, and any
//! other type answers 400 then. A header given twice, not UTF-8 or not of its type answers 400 too. The client leaves
//! out a header whose value is `None`, and refuses a value with a control character, or a space or a tab at either
//! end, which HTTP would drop.
//!
//! The body is the JSON of its type, `application/json`, or one of [`Form`], [`Text`] and [`Bytes`], each sent with
//! its content type. The server answers 415 to a body without the content type it reads, and 400 to one that does not
//! read as its type.
//!
//! # Fixed headers
//!
//! The headers an api asks every request for, a version or a media type, are in its contract:
//! `#[header("name" = value)]` on the api, a group or a route, the value a string or a `&str` constant's name,
//! inherited downward as `#[auth]` is. The nearest to the call wins: an argument of the same header the caller gives,
//! the route, the group, the api, then quoll's own `Accept`. `#[header("name" = none)]` leaves one header the group or
//! the route would take, `#[header(none)]` all of them, and never what quoll writes for the route. A `Content-Type`
//! keeps the media type of the body, adding a parameter, and a request with no body sends none. The client sends
//! them, a [`group!`] taking those of the api that lists it, whose `none` does nothing where that api does not fix
//! the header; the server does not check them, and the OpenAPI document describes each as a header parameter of its
//! value, not required.
//!
//! ```
//! /// The version of the Messages API the contract follows.
//! pub const VERSION: &str = "2023-06-01";
//!
//! quoll::api! {
//!     #[openapi(none)]
//!     #[header("anthropic-version" = VERSION)]
//!     #[header("anthropic-beta" = "files-api-2025-04-14")]
//!     pub api Anthropic = "/v1";
//!
//!     pub group Messages {
//!         POST "/messages" => create(text: String) -> String;
//!
//!         #[header("anthropic-beta" = none)]
//!         POST "/messages/count_tokens" => count_tokens(text: String) -> u32;
//!     }
//! }
//! ```
//!
//! # Fixed fields of the body
//!
//! Two answers of one method and path that another API tells apart by a field of the body are two routes, each with
//! the field it fixes: `#[body("name" = value)]` on a route, the value a bool, a number or a string. The client adds
//! each field at the top of the body, its JSON or a [`Form`], which serializes as a struct or a map, a field of the
//! body's own of that name replaced; the server picks the route whose fields the body holds, then reads the handler's
//! type from the same bytes, which ignores them. Two routes of one method and path compile when a field both fix has
//! two values of one kind, in an api `#[openapi(none)]`; the OpenAPI document describes the fields of a route alone on
//! its path as `const` properties of its body.
//!
//! ```
//! use std::collections::BTreeMap;
//!
//! use quoll::Events;
//!
//! quoll::api! {
//!     #[openapi(none)]
//!     pub api Anthropic = "/v1";
//!
//!     pub group Messages {
//!         #[body("stream" = false)]
//!         POST "/messages" => create(request: BTreeMap<String, String>) -> String;
//!
//!         #[body("stream" = true)]
//!         POST "/messages" => stream(request: BTreeMap<String, String>) -> Events<String>;
//!     }
//! }
//! ```
//!
//! # Results
//!
//! | The route returns | The server answers                                      | The client returns |
//! | ----------------- | ------------------------------------------------------- | ------------------ |
//! | nothing           | 204, no body                                            | `()`               |
//! | `T`               | 200, the JSON of `T`                                    | `T`                |
//! | [`Created<T>`]    | 201, the JSON of `T`, with a `Location` when it has one | `T`                |
//! | [`Accepted<T>`]   | 202, the JSON of `T`                                    | `T`                |
//! | [`Text`]          | 200, `text/plain; charset=utf-8`                        | [`Text`]           |
//! | [`Bytes`]         | 200, their content type                                 | [`Bytes`]          |
//!
//! ```
//! use quoll::{Accepted, Bytes, Created, Text};
//!
//! quoll::api! {
//!     pub api Api = "/api";
//!
//!     pub group Files {
//!         GET    "/files/{id}" => get(id: i64) -> String;
//!         POST   "/files"      => create(name: String) -> Created<i64>;
//!         DELETE "/files/{id}" => remove(id: i64);
//!         POST   "/exports"    => export(name: String) -> Accepted<u64>;
//!         GET    "/readme"     => readme() -> Text;
//!         GET    "/logo"       => logo() -> Bytes;
//!     }
//! }
//! ```
//!
//! The client asks for the result's content type with `Accept`, `*/*` for [`Bytes`], unless the route sends an
//! `Accept` header of its own. It reads JSON from an answer of `application/json` only: an answer of another content
//! type, or JSON that is not the result's, is an error with no status.
//!
//! # Streams
//!
//! A route streams its result with [`Events`], server-sent events, [`JsonLines`], one JSON value per line, or
//! [`ByteStream`], bytes of one content type. Each is one type on both sides, answered 200: the server builds it from a
//! stream and sends its items as they come, and the client reads them as a stream of results.
//!
//! ```
//! use quoll::{ByteStream, Events, JsonLines};
//!
//! quoll::api! {
//!     pub api Api = "/api";
//!
//!     pub group Live {
//!         GET "/notes/{id}/events" => events(id: i64) -> Events<String>;
//!         GET "/export"            => export() -> JsonLines<String>;
//!         GET "/logs"              => logs() -> ByteStream;
//!     }
//! }
//! ```
//!
//! # The types a contract names
//!
//! The contract names [`Created`], [`Accepted`], [`Form`], [`Text`], [`Bytes`], [`Events`], [`JsonLines`] and
//! [`ByteStream`] as it names any type: the module that writes it imports them, `use quoll::{Created, Form};`, or
//! names them by their path, `quoll::Bytes`. A type of another crate with one of these names does not compile there.
//!
//! # Authentication
//!
//! A group or a route says which credential it takes with `#[auth(...)]`, and a route's replaces its group's:
//!
//! - `#[auth(Bearer)]`: the handler receives the caller, `&Caller`; a request with no credential is answered 401;
//! - `#[auth(Bearer | Key)]`: either scheme, up to four, both giving one caller. The first scheme whose credential the
//!   request carries decides, and a wrong one is refused, the next not tried;
//! - `#[auth(Bearer, optional)]`: the handler receives an `Option<&Caller>`, `None` for a request with no credential;
//!   a wrong one is still answered 401;
//! - `#[auth(Bearer, scopes("admin"))]`: the caller implements [`Scoped`], and one that lacks a scope is answered 403
//!   before the handler. A route needs every scope it names;
//! - `#[auth(none)]`, on a route: no credential, whatever its group takes.
//!
//! ```
//! quoll::api! {
//!     pub api Api = "/api";
//!
//!     pub scheme Key = header "X-Api-Key";
//!
//!     #[auth(Bearer)]
//!     pub group Notes {
//!         GET "/notes/{id}" => get(id: i64) -> String;
//!
//!         #[auth(none)]
//!         GET "/public" => public() -> Vec<String>;
//!
//!         #[auth(Bearer, optional)]
//!         GET "/notes/{id}/likes" => likes(id: i64) -> u32;
//!
//!         #[auth(Bearer | Key)]
//!         GET "/count" => count() -> u64;
//!
//!         #[auth(Bearer, scopes("admin"))]
//!         DELETE "/notes/{id}" => remove(id: i64);
//!     }
//! }
//! ```
//!
//! The service implements [`Authenticate`] for each scheme its group takes, which makes the caller of a credential,
//! or answers its error. The client sends the credential through a middleware of [`quoll::client`], as
//! [`client::bearer`], with the feature `client`.
//!
//! # Schemes
//!
//! A scheme is how a credential travels. It is one of:
//!
//! - [`Bearer`], a token in `Authorization`, or [`Basic`], a user and a password there;
//! - a scheme the contract declares, with its visibility and where its credential travels: a header, a cookie, a
//!   query key, or a token of an OpenID Connect provider, sent as a bearer token. It becomes a type of that name,
//!   whose credential is a `String`, and which [`client::key`] sends;
//! - a type of the application's implementing [`Scheme`], as a signature, named in `#[auth(...)]` by its path.
//!
//! ```
//! quoll::api! {
//!     pub api Api = "/api";
//!
//!     /// The key of an operator's scripts.
//!     pub scheme Key = header "X-Api-Key";
//!
//!     /// The session of the web page.
//!     pub scheme Session = cookie "__Host-session";
//!
//!     /// The token of a link sent by mail.
//!     pub scheme Link = query "token";
//!
//!     /// A token of the company's provider.
//!     pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";
//!
//!     #[auth(Key)]
//!     pub group Admin {
//!         GET "/admin/stats" => stats() -> u64;
//!     }
//!
//!     #[auth(Session | Login)]
//!     pub group Notes {
//!         GET "/notes" => list() -> Vec<String>;
//!     }
//!
//!     #[auth(Link)]
//!     pub group Shares {
//!         GET "/shared" => shared() -> String;
//!     }
//!
//!     #[auth(Basic)]
//!     pub group Metrics {
//!         GET "/metrics" => metrics() -> String;
//!     }
//! }
//! ```
//!
//! A header or a query key given twice is a 401, as is an empty one. Of a cookie given twice, the first value that is
//! not empty is read, which a browser that holds it for two paths sends for the longest.
//!
//! A cookie travels with a request a page of another site has a browser send. So a request with the cookie of a
//! cookie scheme whose method is not `GET`, `HEAD` or `OPTIONS` comes from the server's own origin, or is answered
//! 403. The origin is the one its `Sec-Fetch-Site` says, or its `Origin` compared with its `Host`; a request with
//! neither header comes from no browser, and passes. Behind a reverse proxy, the proxy passes the browser's `Host`,
//! as nginx does with `proxy_set_header Host $host`; `X-Forwarded-Host` is not read. Only the server's own origin is
//! trusted for now: a page served from another origin, even of the same site, calls the api with another scheme. The
//! cookie had better be set `SameSite=Lax` or `Strict`, `HttpOnly` and `Secure`, and named with the prefix `__Host-`,
//! which a subdomain cannot set, as a second defense.
//!
//! A route that takes a query scheme reads no other value of its key: the macro refuses `token={...}` in the route's
//! string, but cannot see the fields of a query struct, none of which is to be named `token`.
//!
//! # Groups across modules
//!
//! A large contract splits into [`group!`] blocks, one group in each, in a module of its own, which an api lists by
//! their paths after its prefix. The api serves, calls and describes their routes under its prefix, and checks them
//! at compile time against its own and its other groups'. Schemes are declared in the api alone: a group names one
//! by its path or an import, `#[auth(crate::Key)]`. A group may be listed by two apis.
//!
//! ```
//! mod notes {
//!     quoll::group! {
//!         /// The signed-in user's notes.
//!         #[auth(Bearer | crate::Key)]
//!         pub group Notes {
//!             GET "/notes/{id}" => get(id: i64) -> String;
//!         }
//!     }
//! }
//!
//! quoll::api! {
//!     pub api Api = "/api" { notes::Notes };
//!
//!     /// The key of an operator's scripts.
//!     pub scheme Key = header "X-Api-Key";
//!
//!     #[auth(Key)]
//!     pub group Admin {
//!         GET "/admin/stats" => stats() -> u64;
//!     }
//! }
//! # fn main() {}
//! ```
//!
//! # OpenAPI
//!
//! Whatever the features, `Api::openapi()` gives the contract's OpenAPI 3.1 document, which [`openapi`] describes. So
//! each type a contract names, as a parameter, a body or a result, implements `schemars::JsonSchema`.
//!
//! A contract of a third-party API that is only called says `#[openapi(none)]` on its api. It has no document, and
//! its types, which may come from crates that do not implement `JsonSchema`, need serde alone. A [`group!`] of such
//! a contract says it on its group.
//!
//! ```
//! use serde::{Deserialize, Serialize};
//!
//! /// A repository, as GitHub's API answers it.
//! #[derive(Serialize, Deserialize)]
//! pub struct Repo {
//!     pub full_name: String,
//! }
//!
//! quoll::api! {
//!     /// GitHub's API, which the application only calls.
//!     #[openapi(none)]
//!     pub api GitHub = "";
//!
//!     pub group Repos {
//!         GET "/repos/{owner}/{repo}" => get(owner: &str, repo: &str) -> Repo;
//!     }
//! }
//! ```
//!
//! # Errors
//!
//! Every route returns a [`Result`], whose error is the one [`Error`] of both sides. A handler's error is answered
//! with its status and an RFC 9457 [`Problem`]; a client's is the answer of an error status, or a call that got no
//! answer to read.
//!
//! Another API answers its errors in a shape of its own: `#[error(E)]` on its api, a group or a route says which, the
//! nearest to a route winning, as `#[auth]` does. Its calls give a `quoll::Result<T, E>`, whose error has the `E` the
//! body of an error answer reads as, its [`detail`](Error::detail). `E` implements `Deserialize`, `Display`, which
//! gives the error's message, and `Debug`. A [`group!`] says its own: it cannot take its api's.
//!
//! ```
//! use std::fmt;
//!
//! use serde::Deserialize;
//!
//! /// What GitHub answers on an error.
//! #[derive(Debug, Deserialize)]
//! pub struct GitHubError {
//!     pub message: String,
//! }
//!
//! impl fmt::Display for GitHubError {
//!     fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
//!         f.write_str(&self.message)
//!     }
//! }
//!
//! /// What its OAuth endpoint answers.
//! #[derive(Debug, Deserialize)]
//! pub struct OAuthError {
//!     pub error: String,
//! }
//!
//! impl fmt::Display for OAuthError {
//!     fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
//!         f.write_str(&self.error)
//!     }
//! }
//!
//! quoll::api! {
//!     #[openapi(none)]
//!     #[error(GitHubError)]
//!     pub api GitHub = "";
//!
//!     pub group Repos {
//!         GET "/repos/{owner}/{repo}/topics" => topics(owner: &str, repo: &str) -> Vec<String>;
//!
//!         #[error(OAuthError)]
//!         POST "/login/oauth/access_token" => token(code: String) -> String;
//!     }
//! }
//! ```
//!
//! # Middleware
//!
//! Each request of the client, and each answer of the server, carries the [`Route`] it is for, which a middleware of
//! the application's reads.
//!
//! # What the crate holds
//!
//! - At the root, the contract: [`api!`] and [`group!`], and what it names, its results, its schemes, its error
//!   and its OpenAPI document, in [`openapi`].
//! - [`client`], with the feature `client`: the builder of a client and its middleware, on reqwest-middleware.
//! - [`server`], with the feature `server`: the builder of a server and its layers, on tower.
//! - [`test`], with the feature `server`: a router served on a free port, which a test calls with the real client.
//!
//! What the macro generates for a contract, its traits, its router, its client and its document, is listed in the
//! doc of [`api!`].
//!
//! # Features
//!
//! Both are off by default. A crate that holds the contract alone needs neither: the document is written without
//! them, and the server and the command line turn on the one they need.
//!
//! - `server`: the code of the server, on axum and tokio, with [`server`] and [`test`].
//! - `client`: the code of the client, on reqwest and reqwest-middleware, with [`client`]. It turns on two features of
//!   reqwest: `rustls`, TLS through rustls with the platform's roots, so that a client calls an `https` URL as it is,
//!   and `system-proxy`, the proxy settings of Windows and macOS, beside the `HTTP_PROXY` variables reqwest always
//!   reads. `Api::client_with` keeps the reqwest client the application built, with its own settings.
//!
//! Built without a feature, these docs leave out its modules, and the links to them lead to this section.
//!
#![cfg_attr(feature = "client", doc = "[`client`]: client")]
#![cfg_attr(feature = "client", doc = "[`client::builder()`]: client::builder")]
#![cfg_attr(not(feature = "client"), doc = "[`client`]: crate#features")]
#![cfg_attr(not(feature = "client"), doc = "[`client::builder()`]: crate#features")]
#![cfg_attr(feature = "server", doc = "[`server`]: server")]
#![cfg_attr(feature = "server", doc = "[`server::builder()`]: server::builder")]
#![cfg_attr(feature = "server", doc = "[`test`]: mod@test")]
#![cfg_attr(not(feature = "server"), doc = "[`server`]: crate#features")]
#![cfg_attr(not(feature = "server"), doc = "[`server::builder()`]: crate#features")]
#![cfg_attr(not(feature = "server"), doc = "[`test`]: crate#features")]
#![cfg_attr(feature = "client", doc = "[`quoll::client`]: crate::client")]
#![cfg_attr(feature = "client", doc = "[`client::bearer`]: crate::client::bearer")]
#![cfg_attr(feature = "client", doc = "[`client::key`]: crate::client::key")]
#![cfg_attr(not(feature = "client"), doc = "[`quoll::client`]: crate#features")]
#![cfg_attr(not(feature = "client"), doc = "[`client::bearer`]: crate#features")]
#![cfg_attr(not(feature = "client"), doc = "[`client::key`]: crate#features")]
#![cfg_attr(docsrs, feature(doc_cfg))]

mod accepted;
pub(crate) mod auth;
mod body;
#[cfg(feature = "client")]
mod call;
#[cfg(feature = "client")]
pub mod client;
mod created;
pub(crate) mod error;
pub mod openapi;
#[cfg(any(feature = "client", feature = "server"))]
mod order;
mod problem;
mod response;
mod route;
#[cfg(feature = "server")]
pub mod server;
pub(crate) mod stream;
#[cfg(feature = "server")]
pub mod test;

#[doc(hidden)]
pub mod __private;

pub use accepted::Accepted;
pub use auth::{Authenticate, Basic, BasicCredential, Bearer, Scheme, Scoped};
pub use body::{Bytes, Form, Text};
#[cfg(feature = "client")]
pub use call::Call;
pub use created::Created;
pub use error::{Error, Result};
pub use problem::Problem;
pub use quoll_macros::{api, group};
#[cfg(any(feature = "server", feature = "client"))]
pub(crate) use response::no_content;
pub(crate) use response::sealed::NoBody;
pub use response::{Allows, IntoStatus, InvalidStatus, NoContent, Response, Status};
pub use route::Route;
pub use stream::{ByteStream, Events, JsonLines};
