//! The REST contract: written once with [`api!`], from which the server, the client and the OpenAPI document come.
//!
//! The query of a route is read as serde_html_form reads a form, a field per key. An `Option` may be missing; an
//! empty value, `?page=`, is `None` for a number or a `bool` and `Some("")` for a string. A `Vec` takes every value of
//! its key, and is empty when the key is missing.

mod created;
pub(crate) mod error;

pub use created::Created;
pub use error::{Error, Result};
pub use quoll_macros::api;
