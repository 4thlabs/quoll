//! One value as one piece of text, the way a path parameter, a header and each value of a joined list are: what
//! axum's `Path` reads back, for every integer, a float, a `bool`, a `char`, a string or a unit variant.

mod read;

#[cfg(feature = "client")]
pub(crate) mod write;

pub(crate) use read::{TextError, from_text};
