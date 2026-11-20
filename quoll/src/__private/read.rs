//! The future of a read the generated code awaits, a body or an answer, of a type that has no bound of its own.
//!
//! An `async fn json<T: DeserializeOwned>` returns a future whose type is well formed only when `T` meets the bound,
//! and the `async` block the macro writes holds it across an `.await`: a type that does not meet it is reported twice,
//! on the call, spanned on the type, and again on the block, spanned on the whole contract. A function returning a
//! [`Read`] is checked at its call alone, and the block holds a type that is always well formed.

use std::fmt;
use std::future::Future;
use std::pin::Pin;
use std::task::{Context, Poll};

use serde::de::DeserializeOwned;

use crate::rest::Result;

/// A read of a `T`, boxed: one allocation per call, against the one error a type that cannot be read gets.
#[must_use = "a read does nothing unless it is awaited"]
pub struct Read<T>(Pin<Box<dyn Future<Output = Result<T>> + Send>>);

impl<T> Read<T> {
    /// The read `future` does.
    pub(crate) fn new(future: impl Future<Output = Result<T>> + Send + 'static) -> Self {
        Self(Box::pin(future))
    }
}

impl<T> Future for Read<T> {
    type Output = Result<T>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        self.0.as_mut().poll(cx)
    }
}

impl<T> fmt::Debug for Read<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Read").finish_non_exhaustive()
    }
}

/// A type a route reads from JSON or a form: a body on the server, a result or the item of a stream on the client.
#[diagnostic::on_unimplemented(
    message = "`{Self}` does not implement `serde::Deserialize`, which quoll reads it with",
    label = "no `serde::Deserialize`",
    note = "derive `serde::Deserialize` for `{Self}`, beside `serde::Serialize`: a type of a contract is read on one side and written on the other"
)]
pub trait Deserialized: DeserializeOwned {}

impl<T: DeserializeOwned> Deserialized for T {}
