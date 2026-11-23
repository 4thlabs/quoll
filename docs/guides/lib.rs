//! The user guides of quoll, each a Markdown page of this folder, and the README of the repository: their Rust blocks
//! are compiled and run as the doctests of this crate, so that a guide cannot show code that no longer builds.
//!
//! A block that needs a network is `rust,no_run`, one of tests is `rust,test_harness`, so that its `#[tokio::test]`
//! functions are compiled and run, and one that shows a mistake is `rust,compile_fail`.
//!
//! The README is read as `build.rs` writes it, into `OUT_DIR`: a block of its own that follows
//! `<!-- doctest NAME: WORD... -->` takes the preludes the comment names, hidden, which GitHub does not show. A doctest
//! of it that fails names that copy, whose lines are the README's: the test is named by the line of the comment.

#[cfg(doctest)]
#[doc = include_str!(concat!(env!("OUT_DIR"), "/README.md"))]
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
#[doc = include_str!("another-api.md")]
mod another_api {}

#[cfg(doctest)]
#[doc = include_str!("openapi.md")]
mod openapi {}

#[cfg(doctest)]
#[doc = include_str!("typescript.md")]
mod typescript {}
