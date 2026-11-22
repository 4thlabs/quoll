//! The RFC 9457 problem an API answers with an error.

use http::StatusCode;

/// An RFC 9457 problem: the body of an error answer whose content type is `application/problem+json`, as the server
/// of a contract answers its errors, and the shape of an api's errors unless its contract says another,
/// `#[error(E)]`. The client reads it into [`Error::detail`](crate::rest::Error::detail).
///
/// A member of the wrong type is left out, as the RFC asks; the members of an extension are read with
/// `Error::body_as`, which the feature `client` gives.
// Never `Display` nor `Deserialize`: the client's blanket `Shape` and `From<Error<E>> for Error` are written for the
// types that are both, and would overlap with `Problem`'s own.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Problem {
    pub(crate) kind: String,
    pub(crate) title: Option<String>,
    pub(crate) status: Option<StatusCode>,
    pub(crate) detail: Option<String>,
    pub(crate) instance: Option<String>,
}

impl Problem {
    /// The `type` member, a URI naming the kind of problem: `about:blank` when the problem has none, for a problem
    /// that says no more than its status.
    pub fn kind(&self) -> &str {
        &self.kind
    }

    /// The `title`, a short summary of the kind of problem.
    pub fn title(&self) -> Option<&str> {
        self.title.as_deref()
    }

    /// The `status` the API says it answered, when it says one that is a status.
    pub fn status(&self) -> Option<StatusCode> {
        self.status
    }

    /// The `detail`, what went wrong with this request.
    pub fn detail(&self) -> Option<&str> {
        self.detail.as_deref()
    }

    /// The `instance`, a URI naming this occurrence of the problem.
    pub fn instance(&self) -> Option<&str> {
        self.instance.as_deref()
    }
}
