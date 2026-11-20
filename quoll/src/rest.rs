//! The REST contract: written once with [`api!`], from which the server, the client and the OpenAPI document come.
//!
//! The query of a route is read as serde_html_form reads a form, a field per key. An `Option` may be missing; an
//! empty value, `?page=`, is `None` for a number or a `bool` and `Some("")` for a string. A `Vec` takes every value of
//! its key, and is empty when the key is missing. The client writes it so: a `None` leaves its key out, and a `Vec`
//! repeats it.
//!
//! A query struct, `query filter: NoteFilter`, is the whole query: its fields are the keys, as serde names them, read
//! and written the same way. A `Vec` field that may be missing takes `#[serde(default)]`, as serde_html_form asks. The
//! struct needs no `Default`: an empty query reads when each of its keys may be missing. `Default` is for the callers,
//! which fill the keys they need, `NoteFilter { page: Some(2), ..Default::default() }`.
//!
//! A header, `header("Last-Event-ID") last: Option<String>`, is read from its text as a path parameter is: an integer,
//! a string, a unit variant. An `Option` is `None` when the header is missing, and any other type answers 400 then. A
//! header given twice, not UTF-8 or not of its type answers 400 too. The client leaves out a header whose value is
//! `None`, and refuses a value with a control character, or a space or a tab at either end, which HTTP would drop.
//!
//! The body is the JSON of its type, `application/json`, or one of [`Form`], [`Text`] and [`Bytes`], each sent with
//! its content type. The server answers 415 to a body without the content type it reads. [`Bytes`] carry theirs, which
//! may be any media type but the three a browser sends to another site without asking: a form, a form with files and
//! text.
//!
//! The result is the JSON of its type, answered 200, or 204 with no body for a route that returns nothing. A
//! [`Created`] answers 201 and an [`Accepted`] 202, each with the JSON of its `T`, which is what the client returns. A
//! [`Text`] answers `text/plain; charset=utf-8` and a [`Bytes`] its own content type, `application/octet-stream`
//! unless it says another, both 200. The client reads JSON and text from an answer of their own content type only, and
//! text only in UTF-8: any other answer is an error with no status, as JSON that is not the result's is. It reads
//! bytes of any content type, which they keep. It asks for that content type with `Accept`, `*/*` for bytes, unless
//! the route sends an `Accept` header of its own.
//!
//! A route streams its result with [`Events`], server-sent events, or [`JsonLines`], one JSON value per line, both
//! answered 200: the server sends the values of a stream as they come, and the client reads them as a stream of
//! `Result<T>`. It asks for their content type with `Accept`, and an answer of another one is an error with no status.
//!
//! The contract names `Created`, `Accepted`, `Form`, `Text`, `Bytes`, `Events` and `JsonLines` as it names any type,
//! so that the module that writes it imports them: `use quoll::rest::{self, Created, Form};`. A type of another crate
//! with one of these names does not compile there.
//!
//! A group or a route that takes a credential says so with `#[auth(Bearer)]`, and a route of such a group that takes
//! none with `#[auth(none)]`: the service implements [`Authenticate`], whose caller each handler receives, and the
//! client sends the credential through a middleware, `client::bearer` with the feature `client`.
//!
//! A scheme is [`Bearer`], [`Basic`], one the contract declares, `pub scheme Key = header "X-Api-Key";` (or `cookie
//! "session"`, `query "token"`, `openid "<discovery URL>"`), or a type of the application's implementing [`Scheme`].
//! A declared scheme becomes a type of that name, whose credential is a `String`, and which `client::key::<Key>`
//! sends. `#[auth(Bearer | Key)]` takes either, both giving one caller: the first scheme whose credential the
//! request carries decides, and a wrong one is refused, the next not tried. `#[auth(Bearer, optional)]` gives the
//! handler an `Option<&Caller>`, `None` for a request with no credential and a 401 for a wrong one.
//! `#[auth(Bearer, scopes("repo"))]` answers 403 before the handler when the caller, which implements [`Scoped`],
//! lacks one of the scopes, every one of which it needs.
//!
//! A cookie travels with a request a page of another site has a browser send: a request with the cookie of a cookie
//! scheme whose method is not `GET`, `HEAD` or `OPTIONS` comes from the server's own origin, as its `Sec-Fetch-Site`
//! says, or its `Origin` compared with its `Host`, or is answered 403. Behind a reverse proxy, the proxy passes the
//! browser's `Host`, as nginx does with `proxy_set_header Host $host`; `X-Forwarded-Host` is not read. Only the
//! server's own origin is trusted for now: a page served from another origin, even of the same site, calls the api
//! with another scheme. The cookie had better be set `SameSite=Lax` or `Strict`, `HttpOnly` and `Secure`, and named
//! with the prefix `__Host-`, which a subdomain cannot set, as a second defense. The first value of the cookie that is
//! not empty is read, which a browser that holds it for two paths sends for the longest.
//!
//! A route that takes a query scheme, `scheme Token = query "token";`, reads no other value of its key: the macro
//! refuses `token={...}` in the route's string, but cannot see the fields of a query struct, none of which is to be
//! named `token`.
//!
//! Whatever the features, `Api::openapi()` gives the contract's OpenAPI 3.1 document, which [`openapi`] describes, so
//! that each type a contract names, as a parameter, a body or a result, implements `schemars::JsonSchema`. A contract
//! of a third-party API that is only called, whose models may come from crates that do not implement it, says so on
//! its api with `#[openapi(none)]`: it has no document, and its types need serde alone.

mod accepted;
pub(crate) mod auth;
mod body;
mod created;
pub(crate) mod error;
pub mod openapi;
mod problem;
mod route;
pub(crate) mod stream;

pub use accepted::Accepted;
pub use auth::{Authenticate, Basic, BasicCredential, Bearer, Scheme, Scoped};
pub use body::{Bytes, Form, Text};
pub use created::Created;
pub use error::{Error, Result};
pub use problem::Problem;
pub use quoll_macros::api;
pub use route::Route;
pub use stream::{Events, JsonLines};
