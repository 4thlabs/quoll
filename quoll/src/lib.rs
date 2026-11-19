//! A REST contract for Rust, written once in a macro: an axum server, a reqwest client and an OpenAPI 3.1 document
//! come from it.
//!
//! The crate has two features, each off by default: `server` for the axum side and `client` for the reqwest side.

pub mod rest;

#[doc(hidden)]
pub mod __private;
