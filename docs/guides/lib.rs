//! The user guides of quoll, each a Markdown page of this folder, and the README of the repository: their Rust blocks
//! are compiled and run as the doctests of this crate, so that a guide cannot show code that no longer builds.
//!
//! A block that needs a network is `rust,no_run`, one of tests is `rust,test_harness`, so that its `#[tokio::test]`
//! functions are compiled and run, and one that shows a mistake is `rust,compile_fail`.

#[cfg(doctest)]
#[doc = include_str!("../../README.md")]
mod readme {}

#[cfg(doctest)]
#[doc = include_str!("getting-started.md")]
mod getting_started {}

#[cfg(doctest)]
#[doc = include_str!("contract.md")]
mod contract {}

#[cfg(doctest)]
#[doc = include_str!("server.md")]
mod server {}

#[cfg(doctest)]
#[doc = include_str!("client.md")]
mod client {}

#[cfg(doctest)]
#[doc = include_str!("authentication.md")]
mod authentication {}

#[cfg(doctest)]
#[doc = include_str!("errors.md")]
mod errors {}

#[cfg(doctest)]
#[doc = include_str!("testing.md")]
mod testing {}

#[cfg(doctest)]
#[doc = include_str!("openapi.md")]
mod openapi {}
