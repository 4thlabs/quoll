//! Authentication: how a server reads a credential from a request, and how it makes a caller of it.

use std::future::Future;

use http::request::Parts;

use crate::rest::Result;

mod bearer;

pub use bearer::Bearer;
#[cfg(feature = "client")]
pub(crate) use bearer::is_b64token;

/// How a credential travels in a request: the server reads it with [`read`](Scheme::read), and gives it to the
/// service's [`Authenticate`].
///
/// The crate gives [`Bearer`]. A contract names the scheme a group or a route takes in `#[auth(...)]`.
pub trait Scheme {
    /// The credential the scheme reads: the token of [`Bearer`].
    type Credential;

    /// The credential a request carries.
    ///
    /// `Ok(None)` when the request carries none of this scheme, as a request with no `Authorization` header for
    /// [`Bearer`]: a route that takes the scheme answers 401.
    ///
    /// # Errors
    ///
    /// When the request carries a credential of this scheme that is not well formed. A 401 is answered with a
    /// `WWW-Authenticate` challenge; its detail is sent to the caller, and never holds the credential.
    fn read(request: &Parts) -> Result<Option<Self::Credential>>;
}

/// What a service does with the credential of a scheme: the caller it stands for, which the handler of each route that
/// takes the scheme receives after `&self`.
///
/// A group whose routes take a scheme has a server trait that requires `Authenticate` for it, so a service that does
/// not authenticate does not compile.
///
/// ```
/// use quoll::rest::{Authenticate, Bearer, Error, Result};
///
/// /// Who a request comes from, once its token is checked.
/// struct User {
///     id: i64,
/// }
///
/// struct App;
///
/// impl Authenticate<Bearer> for App {
///     type Caller = User;
///
///     async fn authenticate(&self, token: String) -> Result<User> {
///         match token.as_str() {
///             "ana's token" => Ok(User { id: 1 }),
///             _ => Err(Error::unauthorized("unknown or expired token")),
///         }
///     }
/// }
/// ```
#[diagnostic::on_unimplemented(
    message = "`{Self}` does not implement `Authenticate<{S}>`, which a group it serves takes",
    label = "this service authenticates no `{S}` credential",
    note = "a group or a route with `#[auth({S})]` gives each handler its caller: implement `quoll::rest::Authenticate<{S}>` for `{Self}`, with its `type Caller` and its `async fn authenticate`"
)]
pub trait Authenticate<S: Scheme>: Send + Sync + 'static {
    /// Who a request comes from, once its credential is checked: what the handlers receive.
    type Caller: Send + Sync + 'static;

    /// The caller `credential` stands for. It runs before the handler, and before the request's body is read.
    ///
    /// An implementation writes `async fn`.
    ///
    /// # Errors
    ///
    /// The error is answered as a handler's is, with the request's handler never called: [`Error::unauthorized`]
    /// for a credential that stands for no caller, answered 401 with a `WWW-Authenticate` challenge;
    /// [`Error::internal`] for a failure of the server's own, as a database that does not answer, answered 500 so
    /// that the client does not take its credential for a wrong one.
    ///
    /// [`Error::unauthorized`]: crate::rest::Error::unauthorized
    /// [`Error::internal`]: crate::rest::Error::internal
    fn authenticate(
        &self,
        credential: S::Credential,
    ) -> impl Future<Output = Result<Self::Caller>> + Send;
}
