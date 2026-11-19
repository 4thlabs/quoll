//! What a request is for: the route of the contract it calls, which middleware reads.

use http::Method;

/// The route of the contract a request calls. The client puts it in the extensions of each request it sends, where a
/// reqwest-middleware middleware reads it, to name a span after the operation or to send a credential to the routes
/// that take it.
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

    /// The names of the authentication schemes the route accepts, as the contract writes them; empty for a route
    /// that takes none.
    pub fn schemes(&self) -> &'static [&'static str] {
        self.schemes
    }
}
