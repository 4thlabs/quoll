//! The REST contract: written once with [`api!`], from which the server, the client and the OpenAPI document come.
//!
//! This page is a map of what a contract holds, a form at a time. The syntax as a whole, and the items the macro
//! generates, are in the doc of [`api!`]; a contract split across modules is in [`group!`].
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
//! use quoll::rest;
//!
//! rest::api! {
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
//! use quoll::rest::{self, Bytes, Form, Text};
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
//! rest::api! {
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
//! is refused by the client, before sending. A field of a query struct names a type of [`join`] in serde's `with`
//! instead, `#[serde(with = "rest::join::Comma::<u32>")]`.
//!
//! ```
//! use quoll::rest;
//!
//! rest::api! {
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
//! them, a [`group!`] taking those of the api that lists it; the server does not check them, and the OpenAPI document
//! describes each as a header parameter of its value, not required.
//!
//! ```
//! use quoll::rest;
//!
//! /// The version of the Messages API the contract follows.
//! pub const VERSION: &str = "2023-06-01";
//!
//! rest::api! {
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
//! use quoll::rest::{self, Accepted, Bytes, Created, Text};
//!
//! rest::api! {
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
//! use quoll::rest::{self, ByteStream, Events, JsonLines};
//!
//! rest::api! {
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
//! [`ByteStream`] as it names any type: the module that writes it imports them, `use quoll::rest::{self, Created,
//! Form};`, or names them by their path, `rest::Bytes`. A type of another crate with one of these names does not
//! compile there.
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
//! use quoll::rest;
//!
//! rest::api! {
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
//! use quoll::rest;
//!
//! rest::api! {
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
//!     use quoll::rest;
//!
//!     rest::group! {
//!         /// The signed-in user's notes.
//!         #[auth(Bearer | crate::Key)]
//!         pub group Notes {
//!             GET "/notes/{id}" => get(id: i64) -> String;
//!         }
//!     }
//! }
//!
//! use quoll::rest;
//!
//! rest::api! {
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
//! use quoll::rest;
//! use serde::{Deserialize, Serialize};
//!
//! /// A repository, as GitHub's API answers it.
//! #[derive(Serialize, Deserialize)]
//! pub struct Repo {
//!     pub full_name: String,
//! }
//!
//! rest::api! {
//!     /// GitHub's API, which the application only calls.
//!     #[openapi(none)]
//!     pub api GitHub = "";
//!
//!     pub group Repos {
//!         GET "/repos/{owner}/{repo}" => get(owner: String, repo: String) -> Repo;
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
//! nearest to a route winning, as `#[auth]` does. Its calls give a `rest::Result<T, E>`, whose error has the `E` the
//! body of an error answer reads as, its [`detail`](Error::detail). `E` implements `Deserialize`, `Display`, which
//! gives the error's message, and `Debug`. A [`group!`] says its own: it cannot take its api's.
//!
//! ```
//! use std::fmt;
//!
//! use quoll::rest;
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
//! rest::api! {
//!     #[openapi(none)]
//!     #[error(GitHubError)]
//!     pub api GitHub = "";
//!
//!     pub group Repos {
//!         GET "/repos/{owner}/{repo}/topics" => topics(owner: String, repo: String) -> Vec<String>;
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
#![cfg_attr(feature = "client", doc = "[`quoll::client`]: crate::client")]
#![cfg_attr(feature = "client", doc = "[`client::bearer`]: crate::client::bearer")]
#![cfg_attr(feature = "client", doc = "[`client::key`]: crate::client::key")]
#![cfg_attr(not(feature = "client"), doc = "[`quoll::client`]: crate#features")]
#![cfg_attr(not(feature = "client"), doc = "[`client::bearer`]: crate#features")]
#![cfg_attr(not(feature = "client"), doc = "[`client::key`]: crate#features")]

mod accepted;
pub(crate) mod auth;
mod body;
#[cfg(feature = "client")]
mod call;
mod created;
pub(crate) mod error;
pub mod join;
pub mod openapi;
mod problem;
mod response;
mod route;
pub(crate) mod stream;

pub use accepted::Accepted;
pub use auth::{Authenticate, Basic, BasicCredential, Bearer, Scheme, Scoped};
pub use body::{Bytes, Form, Text};
#[cfg(feature = "client")]
pub use call::Call;
pub use created::Created;
pub use error::{Error, Result};
pub use problem::Problem;
pub use quoll_macros::{api, group};
pub use response::{InvalidStatus, Response, Status};
pub use route::Route;
pub use stream::{ByteStream, Events, JsonLines};
