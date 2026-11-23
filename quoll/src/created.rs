//! The result of a route that creates what it returns.

use std::fmt;
use std::marker::PhantomData;

/// What a route answers when it creates a `T`, as the contract writes it: a 201, with `T` in JSON.
///
/// It is only written in the contract, `-> Created<Note>`: the handler returns a [`Response<Note>`], as every
/// handler does, to which quoll gives the 201, and with a `Location` when it says where the value is read. The client
/// returns the `T`.
///
/// ```
/// use http::header::LOCATION;
/// use quoll::{Created, Response};
///
/// quoll::api! {
///     pub api Api = "/api";
///
///     pub group Notes {
///         POST "/notes" => create(title: String) -> Created<i64>;
///     }
/// }
///
/// fn created(id: i64) -> Response<i64> {
///     Response::new(id).header(LOCATION, format!("/api/notes/{id}"))
/// }
/// # let _ = created(3);
/// ```
///
/// [`Response<Note>`]: crate::Response
pub struct Created<T> {
    _value: PhantomData<fn() -> T>,
}

impl<T> fmt::Debug for Created<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Created")
    }
}
