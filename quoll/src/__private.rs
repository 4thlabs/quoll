//! What only the code `quoll::api!` writes calls. Not part of the API: it may change in any version.
//!
//! The macro cannot read the features of `quoll`, and a `#[cfg(feature = "server")]` in what it writes would read the
//! features of the user's crate. So the code of each side goes through a macro defined here, which writes it out when
//! `quoll` has the feature and drops it when it does not.

pub use http;

#[cfg(feature = "server")]
pub use axum;

#[cfg(feature = "client")]
pub use reqwest_middleware;

pub use serde;

pub mod auth;
pub(crate) mod content_type;
pub mod fields;
pub mod group;
pub mod headers;
pub mod join;
pub mod known;
pub mod openapi;
pub(crate) mod text;

#[cfg(any(feature = "server", feature = "client"))]
pub mod read;

#[cfg(feature = "server")]
pub mod server;

#[cfg(feature = "client")]
pub mod client;

/// Writes out its input: `quoll` has the feature `server`.
#[cfg(feature = "server")]
#[doc(hidden)]
#[macro_export]
macro_rules! __server {
    ($($item:tt)*) => {
        $($item)*
    };
}

/// Drops its input: `quoll` does not have the feature `server`.
#[cfg(not(feature = "server"))]
#[doc(hidden)]
#[macro_export]
macro_rules! __server {
    ($($item:tt)*) => {};
}

pub use crate::__server as server;

/// Writes out its input: `quoll` has the feature `client`.
#[cfg(feature = "client")]
#[doc(hidden)]
#[macro_export]
macro_rules! __client {
    ($($item:tt)*) => {
        $($item)*
    };
}

/// Drops its input: `quoll` does not have the feature `client`.
#[cfg(not(feature = "client"))]
#[doc(hidden)]
#[macro_export]
macro_rules! __client {
    ($($item:tt)*) => {};
}

pub use crate::__client as client;

/// Drops its input: `quoll` has the feature `client`, whose code checks what the input would.
#[cfg(feature = "client")]
#[doc(hidden)]
#[macro_export]
macro_rules! __without_client {
    ($($item:tt)*) => {};
}

/// Writes out its input: `quoll` does not have the feature `client`, whose code would check what the input does.
#[cfg(not(feature = "client"))]
#[doc(hidden)]
#[macro_export]
macro_rules! __without_client {
    ($($item:tt)*) => {
        $($item)*
    };
}

pub use crate::__without_client as without_client;
