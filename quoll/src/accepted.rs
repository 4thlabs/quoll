//! The result of a route that accepts work it has not done yet.

use std::fmt;
use std::marker::PhantomData;

/// What a route answers when it accepts work to do later, as the contract writes it: a 202, with `T` in JSON, which
/// says what was accepted, as a job and where to follow it.
///
/// It is only written in the contract, `-> Accepted<Export>`: the handler returns a [`Response<Export>`], as every
/// handler does, to which quoll gives the 202. The client returns the `T`.
///
/// ```
/// use quoll::Accepted;
///
/// quoll::api! {
///     pub api Api = "/api";
///
///     pub group Exports {
///         POST "/exports" => export(name: String) -> Accepted<u64>;
///     }
/// }
/// ```
///
/// [`Response<Export>`]: crate::Response
pub struct Accepted<T> {
    _value: PhantomData<fn() -> T>,
}

impl<T> fmt::Debug for Accepted<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Accepted")
    }
}
