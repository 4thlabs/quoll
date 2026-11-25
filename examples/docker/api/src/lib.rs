//! A contract of the Docker Engine API, which this example only calls: the routes of its command line, and the few
//! more its test against a real daemon needs.

mod api;
mod containers;
mod error;
mod exec;
mod images;
mod system;

pub use api::*;
pub use containers::*;
pub use error::*;
pub use exec::*;
pub use images::*;
pub use system::*;
