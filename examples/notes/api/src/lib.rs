//! The contract of the notes example, which the server implements and its clients call.

mod admin;
mod api;
mod auth;
mod notes;

pub use admin::*;
pub use api::*;
pub use auth::*;
pub use notes::*;
