//! Authentication: the traits of a scheme, how a server reads its credential and a client writes it, and the helpers
//! that read and write the grammar of `Authorization`.
//!
//! The traits are re-exported at the root of the crate: [`Scheme`], [`ServerScheme`], [`ClientScheme`],
//! [`Authenticate`] and [`Scoped`]. This module adds what a scheme of the application's own is written with:
//!
//! - on the server, [`authorization`], which gives the [`Credentials`] of an auth-scheme, read as a `token68` or as
//!   RFC 9110 `#auth-param`s;
//! - on the client, [`Outgoing`], the request a [`ClientScheme`] writes on, and [`Unsendable`], why it refuses;
//! - on both, [`is_token`] and [`is_token68`], the RFC's two words.

use std::future::Future;

use http::HeaderValue;
use http::request::Parts;

use crate::Result;
use crate::openapi::SecurityScheme;

mod basic;
mod bearer;
mod credentials;
mod outgoing;

pub use basic::{Basic, BasicCredential};
pub use bearer::Bearer;
pub use credentials::{Credentials, authorization, is_token, is_token68};
#[cfg(feature = "client")]
pub(crate) use outgoing::Added;
pub use outgoing::{Outgoing, Unsendable};

/// What both sides know of a scheme: its name.
///
/// A scheme is a type implementing `Scheme` and one side or both: [`ServerScheme`], how a server reads its credential,
/// and [`ClientScheme`], how a client writes it. `#[auth(...)]` asks of it the sides its api has: both by default,
/// [`ClientScheme`] alone with `#[server(none)]`, and [`ServerScheme`] alone with `#[client(none)]`.
///
/// The crate gives [`Bearer`] and [`Basic`], which implement both; a contract declares the others it takes, `pub
/// scheme Key = header "X-Api-Key";`, which implement the sides of their api. Any other is a type of the
/// application's, named in `#[auth(...)]` as the others are.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not a scheme: `#[auth(...)]` takes `Bearer`, `Basic`, a scheme the contract declares, or a type implementing `quoll::Scheme`",
    label = "not a scheme",
    note = "declare it in the contract, as `pub scheme Key = header \"X-Api-Key\";`, or implement `quoll::Scheme` for `{Self}`, then `quoll::ServerScheme` or `quoll::ClientScheme`, as its api serves or calls"
)]
pub trait Scheme: 'static {
    /// The scheme's name: what [`Route::schemes`](crate::Route::schemes) holds for a route that takes it, what the
    /// OpenAPI document names it, and the scheme of its `WWW-Authenticate` challenge unless
    /// [`CHALLENGE`](ServerScheme::CHALLENGE) says another. Each scheme of an api has its own.
    const NAME: &'static str;
}

/// How a server reads the credential of a scheme, answers a request with none, and describes it in the OpenAPI
/// document. A scheme an api only calls, `#[server(none)]`, does not implement it.
///
/// ```
/// use http::request::Parts;
/// use quoll::{Error, Result, Scheme, ServerScheme, openapi::SecurityScheme};
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
///     const NAME: &'static str = "Hmac";
/// }
///
/// impl ServerScheme for Hmac {
///     type Credential = Signed;
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
    message = "`{Self}` is not a scheme a server reads, which an api that serves takes",
    label = "no server reads `{Self}`",
    note = "implement `quoll::ServerScheme` for `{Self}`, with its `read` and its `openapi`; an api that only calls writes `#[server(none)]`"
)]
pub trait ServerScheme: Scheme {
    /// The credential the server reads: the token of [`Bearer`]. It is `Send`, as the service's `authenticate` takes
    /// it across an `await`.
    type Credential: Send + 'static;

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
    /// [`NAME`](Scheme::NAME).
    fn openapi() -> SecurityScheme;
}

/// How a client writes the credential of a scheme on a request: what [`client::key`] sends. A scheme an api only
/// serves, `#[client(none)]`, does not implement it.
///
/// The client calls [`write`](ClientScheme::write) on the requests whose route takes the scheme, to the api's own
/// origin, and on no other: a route with `#[auth(none)]` or a request sent to another host never gets the credential.
/// What it writes through [`Outgoing`] is marked sensitive.
///
/// A redirect to another origin gets nothing when the client follows it with `client::redirects`, which
/// `client::builder()` adds once it is given a credential. A reqwest client that follows redirects itself, as one
/// given to `Api::client_with` does by default, strips only `Authorization`, `Cookie` and `Proxy-Authorization` on a
/// hop to another host: any other header that `write` adds, as `X-Signature`, goes with it.
///
/// ```
/// use quoll::auth::{Outgoing, Unsendable};
/// use quoll::{ClientScheme, Scheme};
///
/// /// A Jellyfin API key: `Authorization: MediaBrowser Token="<key>"`.
/// struct MediaBrowser;
///
/// impl Scheme for MediaBrowser {
///     const NAME: &'static str = "MediaBrowser";
/// }
///
/// impl ClientScheme for MediaBrowser {
///     type Secret = String;
///
///     fn write(key: &String, request: &mut Outgoing<'_>) -> Result<(), Unsendable> {
///         request.authorization_params("MediaBrowser", &[("Token", key)])
///     }
/// }
/// ```
///
#[cfg_attr(feature = "client", doc = "[`client::key`]: crate::client::key")]
#[cfg_attr(not(feature = "client"), doc = "[`client::key`]: crate#features")]
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not a scheme a client writes, which an api that calls takes",
    label = "no client writes `{Self}`",
    note = "implement `quoll::ClientScheme` for `{Self}`, with its `write`, for `client::key` to send it; an api that only serves writes `#[client(none)]`"
)]
pub trait ClientScheme: Scheme {
    /// What the client is given to write the credential: the token of [`Bearer`], the key a signature is made with.
    type Secret: Send + Sync + 'static;

    /// Writes the credential of `secret` on `request`.
    ///
    /// # Errors
    ///
    /// When the credential cannot be sent as the server would read it: the call fails with the error, which never
    /// holds the credential, and is not sent.
    fn write(
        secret: &Self::Secret,
        request: &mut Outgoing<'_>,
    ) -> std::result::Result<(), Unsendable>;
}

/// A caller that holds scopes, which a route with `#[auth(Bearer, scopes("repo"))]` requires: the router answers 403
/// before the handler when the caller lacks one of them.
///
/// ```
/// use quoll::Scoped;
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
    message = "`{Self}` does not implement `quoll::Scoped`, which a route with `scopes(...)` needs of its caller",
    label = "this caller holds no scopes",
    note = "implement `quoll::Scoped` for `{Self}`, whose `has_scope` says whether the caller holds a scope"
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
/// use quoll::{Authenticate, Bearer, Error, Result};
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
    note = "a group or a route with `#[auth({S})]` gives each handler its caller: implement `quoll::Authenticate<{S}>` for `{Self}`, with its `type Caller` and its `async fn authenticate`"
)]
pub trait Authenticate<S: ServerScheme>: Send + Sync + 'static {
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
    /// [`Error::unauthorized`]: crate::Error::unauthorized
    /// [`Error::internal`]: crate::Error::internal
    fn authenticate(
        &self,
        credential: S::Credential,
    ) -> impl Future<Output = Result<Self::Caller>> + Send;
}
