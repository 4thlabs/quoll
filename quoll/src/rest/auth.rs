//! Authentication: how a server reads a credential from a request, and how it makes a caller of it.

use std::future::Future;

use http::HeaderValue;
use http::request::Parts;

use crate::rest::Result;
use crate::rest::openapi::SecurityScheme;

mod basic;
mod bearer;

pub use basic::{Basic, BasicCredential};
pub use bearer::Bearer;
#[cfg(feature = "client")]
pub(crate) use bearer::is_b64token;

/// How a credential travels in a request: the server reads it with [`read`](Scheme::read), and gives it to the
/// service's [`Authenticate`].
///
/// The crate gives [`Bearer`] and [`Basic`]; a contract declares the others it takes, `pub scheme Key = header
/// "X-Api-Key";`, and names the scheme a group or a route takes in `#[auth(...)]`. Any other is a type of the
/// application implementing `Scheme`, named in `#[auth(...)]` as the others are:
///
/// ```
/// use http::request::Parts;
/// use quoll::rest::openapi::SecurityScheme;
/// use quoll::rest::{Error, Result, Scheme};
///
/// /// A request signed with a key the server knows: `X-Signature: <key id>:<signature>`.
/// struct Hmac;
///
/// /// What a signed request carries, which the service checks.
/// struct Signed {
///     key_id: String,
///     signature: String,
/// }
///
/// impl Scheme for Hmac {
///     type Credential = Signed;
///
///     const NAME: &'static str = "Hmac";
///
///     fn read(request: &Parts) -> Result<Option<Signed>> {
///         let Some(value) = request.headers.get("X-Signature") else {
///             return Ok(None);
///         };
///         let value = value
///             .to_str()
///             .map_err(|_| Error::unauthorized("the signature is not text"))?;
///         let Some((key_id, signature)) = value.split_once(':') else {
///             return Err(Error::unauthorized("the signature is `<key id>:<signature>`"));
///         };
///
///         Ok(Some(Signed { key_id: key_id.into(), signature: signature.into() }))
///     }
///
///     fn openapi() -> SecurityScheme {
///         SecurityScheme::header("X-Signature").with_description("`<key id>:<signature>`")
///     }
/// }
/// ```
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not a scheme: `#[auth(...)]` takes `Bearer`, `Basic`, a scheme the contract declares, or a type implementing `quoll::rest::Scheme`",
    label = "not a scheme",
    note = "declare it in the contract, as `pub scheme Key = header \"X-Api-Key\";`, or implement `quoll::rest::Scheme` for `{Self}`"
)]
pub trait Scheme: 'static {
    /// The credential the scheme reads: the token of [`Bearer`]. It is `Send`, as the service's `authenticate` takes
    /// it across an `await`.
    type Credential: Send + 'static;

    /// The scheme's name: what [`Route::schemes`](crate::rest::Route::schemes) holds for a route that takes it, and
    /// the scheme of its `WWW-Authenticate` challenge unless [`CHALLENGE`](Scheme::CHALLENGE) says another. Each
    /// scheme of an api has its own.
    const NAME: &'static str;

    /// The `WWW-Authenticate` of a 401 on a route that takes the scheme: its [`NAME`](Scheme::NAME) unless it says
    /// another. A constant, so that a challenge that is not a header's value does not compile.
    const CHALLENGE: HeaderValue = HeaderValue::from_static(Self::NAME);

    /// The credential a request carries.
    ///
    /// `Ok(None)` when the request carries none of this scheme, as a request with no `Authorization` header for
    /// [`Bearer`]: a route that takes the scheme answers 401, and one that takes others tries the next.
    ///
    /// # Errors
    ///
    /// When the request carries a credential of this scheme that is not well formed. A 401 is answered with a
    /// `WWW-Authenticate` challenge; its detail is sent to the caller, and never holds the credential.
    fn read(request: &Parts) -> Result<Option<Self::Credential>>;

    /// How the OpenAPI document describes the scheme, under `components/securitySchemes` by its
    /// [`NAME`](Scheme::NAME): `SecurityScheme::bearer()` for [`Bearer`], a key in a header for a scheme that reads
    /// one.
    fn openapi() -> SecurityScheme;
}

/// A caller that holds scopes, which a route with `#[auth(Bearer, scopes("repo"))]` requires: the router answers 403
/// before the handler when the caller lacks one of them.
///
/// ```
/// use quoll::rest::Scoped;
///
/// /// Who a request comes from, with the scopes its token grants.
/// struct User {
///     scopes: Vec<String>,
/// }
///
/// impl Scoped for User {
///     fn has_scope(&self, scope: &str) -> bool {
///         self.scopes.iter().any(|granted| granted == scope)
///     }
/// }
/// ```
#[diagnostic::on_unimplemented(
    message = "`{Self}` does not implement `quoll::rest::Scoped`, which a route with `scopes(...)` needs of its caller",
    label = "this caller holds no scopes",
    note = "implement `quoll::rest::Scoped` for `{Self}`, whose `has_scope` says whether the caller holds a scope"
)]
pub trait Scoped {
    /// Whether the caller holds `scope`, one of the scopes a route names in its `#[auth(...)]`.
    fn has_scope(&self, scope: &str) -> bool;
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
