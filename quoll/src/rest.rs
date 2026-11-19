//! The REST contract: written once with [`api!`], from which the server, the client and the OpenAPI document come.
//!
//! The query of a route is read as serde_html_form reads a form, a field per key. An `Option` may be missing; an
//! empty value, `?page=`, is `None` for a number or a `bool` and `Some("")` for a string. A `Vec` takes every value of
//! its key, and is empty when the key is missing. The client writes it so: a `None` leaves its key out, and a `Vec`
//! repeats it.
//!
//! A group or a route that takes a credential says so with `#[auth(Bearer)]`, and a route of such a group that takes
//! none with `#[auth(none)]`: the service implements [`Authenticate`], whose caller each handler receives, and the
//! client sends the credential through a middleware, `client::bearer` with the feature `client`.

mod auth;
#[cfg(feature = "client")]
pub mod client;
mod created;
pub(crate) mod error;
mod problem;
mod route;
#[cfg(feature = "server")]
pub mod test;

pub use auth::{Authenticate, Bearer, Scheme};
pub use created::Created;
pub use error::{Error, Result};
pub use problem::Problem;
pub use quoll_macros::api;
pub use route::Route;
