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
