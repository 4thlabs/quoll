//! What the generated code reads of the schemes the crate gives, which it names by their type.

use http::HeaderValue;

use crate::rest::{Bearer, Scheme};

/// A scheme the crate gives: its name, which `rest::Route::schemes` holds, and the challenge of a 401 on a route that
/// takes it. The one place the name is written.
pub trait Given: Scheme {
    /// The scheme's name, `Bearer`.
    const NAME: &'static str;

    /// The `WWW-Authenticate` of a 401 on a route that takes the scheme. A const, so that a name that is not a
    /// header's value does not compile.
    const CHALLENGE: HeaderValue;
}

impl Given for Bearer {
    const NAME: &'static str = "Bearer";
    const CHALLENGE: HeaderValue = HeaderValue::from_static(Self::NAME);
}
