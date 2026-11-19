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
//! its content type. The server answers 415 to a body without the content type it reads.
//!
//! The result is the JSON of its type, answered 200, or 204 with no body for a route that returns nothing. A
//! [`Created`] answers 201 and an [`Accepted`] 202, each with the JSON of its `T`, which is what the client returns. A
//! [`Text`] answers `text/plain; charset=utf-8` and a [`Bytes`] `application/octet-stream`, both 200. The client reads
//! each from an answer of its own content type only, and text only in UTF-8: any other answer is an error with no
//! status, as JSON that is not the result's is.
//!
//! The contract names `Created`, `Accepted`, `Form`, `Text` and `Bytes` as it names any type, so that the module that
//! writes it imports them: `use quoll::rest::{self, Created, Form};`. A type of another crate with one of these names
//! does not compile there.
//!
//! A group or a route that takes a credential says so with `#[auth(Bearer)]`, and a route of such a group that takes
//! none with `#[auth(none)]`: the service implements [`Authenticate`], whose caller each handler receives, and the
//! client sends the credential through a middleware, `client::bearer` with the feature `client`.

mod accepted;
mod auth;
mod body;
#[cfg(feature = "client")]
pub mod client;
mod created;
pub(crate) mod error;
mod problem;
mod route;
#[cfg(feature = "server")]
pub mod test;

pub use accepted::Accepted;
pub use auth::{Authenticate, Bearer, Scheme};
pub use body::{Bytes, Form, Text};
pub use created::Created;
pub use error::{Error, Result};
pub use problem::Problem;
pub use quoll_macros::api;
pub use route::Route;
