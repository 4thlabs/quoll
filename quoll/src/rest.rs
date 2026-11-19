//! The REST contract: written once with [`api!`], from which the server, the client and the OpenAPI document come.
//!
//! The query of a route is read as serde_html_form reads a form, a field per key. An `Option` may be missing; an
//! empty value, `?page=`, is `None` for a number or a `bool` and `Some("")` for a string. A `Vec` takes every value of
//! its key, and is empty when the key is missing. The client writes it so: a `None` leaves its key out, and a `Vec`
//! repeats it.

mod created;
pub(crate) mod error;
mod problem;
mod route;

pub use created::Created;
pub use error::{Error, Result};
pub use problem::Problem;
pub use quoll_macros::api;
pub use route::Route;
