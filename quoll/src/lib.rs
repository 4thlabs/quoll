//! A REST contract for Rust, written once in a macro: an axum server, a reqwest client and an OpenAPI 3.1 document
//! come from it.
//!
//! The crate has two features, each off by default: `server` for the axum side and `client` for the reqwest side.
//!
//! The client turns on no TLS in reqwest: an application that calls `https` URLs turns on the TLS of its choice in
//! its own dependency on reqwest, such as `reqwest = { version = "0.13", features = ["rustls"] }`.

pub mod rest;

#[doc(hidden)]
pub mod __private;
