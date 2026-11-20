//! What a request is for: the route of the contract it calls, which middleware reads.

use std::any::TypeId;

use http::Method;

use crate::rest::Scheme;

/// The route of the contract a request calls. The client puts it in the extensions of each request it sends, where a
/// reqwest-middleware middleware reads it, to name a span after the operation or to send a credential to the routes
/// that take it. The server puts it in the extensions of each answer of a route, where a tower layer reads it, to
/// count the answers of each operation; an answer no route made has none, as axum's 404 and the 503 of
/// `server::timeout`.
///
/// ```
/// use quoll::rest::Route;
///
/// /// The operation a request calls, as a middleware reads it from the request's extensions.
/// fn operation(extensions: &http::Extensions) -> Option<&'static str> {
///     extensions.get::<Route>().map(Route::operation_id)
/// }
/// # assert_eq!(operation(&http::Extensions::new()), None);
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Route {
    pub(crate) operation_id: &'static str,
    pub(crate) group: &'static str,
    pub(crate) method: Method,
    pub(crate) path: &'static str,
    pub(crate) schemes: &'static [&'static str],
    pub(crate) scheme_types: &'static [TypeId],
    /// Whether the result is a stream, whose body `client::timeout` leaves without a deadline.
    pub(crate) streams: bool,
}

impl Route {
    /// The operation id: the group's name in snake case, `_`, and the route's function, `notes_get`. OpenAPI names the
    /// operation so.
    pub fn operation_id(&self) -> &'static str {
        self.operation_id
    }

    /// The group's name, as the contract writes it: `Notes`.
    pub fn group(&self) -> &'static str {
        self.group
    }

    /// The HTTP method.
    pub fn method(&self) -> &Method {
        &self.method
    }

    /// The path under the api's prefix, with its parameters as the contract writes them and without the query:
    /// `/api/notes/{id}`.
    pub fn path(&self) -> &'static str {
        self.path
    }

    /// The names of the schemes the route takes, each its [`Scheme::NAME`], in the order its `#[auth(...)]` or its
    /// group's writes them: `["Bearer", "Key"]`, or none for a route that takes no credential.
    ///
    /// Two contracts may each declare a scheme of one name, as `Key`: a middleware that sends a credential asks
    /// [`takes`](Route::takes), which tells them apart.
    pub fn schemes(&self) -> &'static [&'static str] {
        self.schemes
    }

    /// Whether the route takes the scheme `S`: a middleware that sends its credential asks it, so that the routes that
    /// do not take it never get it.
    ///
    /// ```
    /// use quoll::rest::{Bearer, Route};
    ///
    /// /// Whether a request is for a route that takes a bearer token.
    /// fn takes_bearer(extensions: &http::Extensions) -> bool {
    ///     extensions.get::<Route>().is_some_and(Route::takes::<Bearer>)
    /// }
    /// # assert!(!takes_bearer(&http::Extensions::new()));
    /// ```
    pub fn takes<S: Scheme>(&self) -> bool {
        self.scheme_types.contains(&TypeId::of::<S>())
    }
}
