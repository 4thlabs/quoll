//! The REST contract: written once with [`api!`], from which the server, the client and the OpenAPI document come.

mod created;
pub(crate) mod error;

pub use created::Created;
pub use error::{Error, Result};
pub use quoll_macros::api;
