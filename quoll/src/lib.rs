//! A REST contract for Rust, written once in a macro: an axum server, a reqwest client and an OpenAPI 3.1 document
//! come from it.
//!
//! The crate has two features, each off by default: `server` for the axum side and `client` for the reqwest side.
//!
//! The feature `client` turns on two features of reqwest, so that `Api::client` calls an `https` URL as it is:
//! `rustls`, TLS through rustls with the platform's roots, and `system-proxy`, the proxy settings of Windows and macOS,
//! beside the `HTTP_PROXY` variables reqwest always reads. `Api::client_with` keeps the client the application built.

pub mod rest;

#[doc(hidden)]
pub mod __private;
