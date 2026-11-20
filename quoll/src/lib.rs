//! A REST contract for Rust, written once in a macro: an axum server, a reqwest client and an OpenAPI 3.1 document
//! come from it.
//!
//! The crate has two features, each off by default: `server` for the axum side and `client` for the reqwest side.
//!
//! - [`rest`] is the contract, written with [`rest::api!`], and what it names: its types, its error, its schemes.
//! - `client`, with the feature `client`, builds the client of a contract, with the middleware of reqwest-middleware
//!   that quoll gives, put in the order they work in, and any other.
//! - `server`, with the feature `server`, serves the router of a contract, with the tower layers that quoll gives, put
//!   in the order they work in, and any other, until it is told to stop; `test` serves it for a test.
//!
//! The feature `client` turns on two features of reqwest, so that `Api::client` calls an `https` URL as it is:
//! `rustls`, TLS through rustls with the platform's roots, and `system-proxy`, the proxy settings of Windows and macOS,
//! beside the `HTTP_PROXY` variables reqwest always reads. `Api::client_with` keeps the client the application built.

#[cfg(feature = "client")]
pub mod client;
#[cfg(any(feature = "client", feature = "server"))]
mod order;
pub mod rest;
#[cfg(feature = "server")]
pub mod server;
#[cfg(feature = "server")]
pub mod test;

#[doc(hidden)]
pub mod __private;
