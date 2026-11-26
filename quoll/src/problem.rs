//! The RFC 9457 problem an API answers with an error.

use http::StatusCode;
use serde::Serialize;
use serde_json::{Map, Value};

/// An RFC 9457 problem: the body of an error answer whose content type is `application/problem+json`, as the server
/// of a contract answers its errors, and the shape of an api's errors unless its contract says another,
/// `#[error(E)]`. The client reads it into [`Error::detail`](crate::Error::detail).
///
/// A member of the wrong type is left out, as the RFC asks; the members of an extension are read with
/// `Error::body_as`, which the feature `client` gives. A handler gives its problem a `type` and extension members
/// with [`Error::with_type`](crate::Error::with_type) and [`Error::with_member`](crate::Error::with_member).
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

/// The problem quoll's server answers for an error, in the order the RFC gives its members, then the extension
/// members of the handler's.
#[derive(Serialize)]
pub(crate) struct Answer<'a> {
    #[serde(rename = "type")]
    pub(crate) kind: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) title: Option<&'static str>,
    #[serde(serialize_with = "status_code")]
    pub(crate) status: StatusCode,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub(crate) detail: Option<&'a str>,
    #[serde(flatten)]
    pub(crate) members: Option<&'a Map<String, Value>>,
}

impl Answer<'_> {
    /// The JSON of the problem.
    pub(crate) fn to_json(&self) -> Vec<u8> {
        serde_json::to_vec(self).expect(
            "a problem holds strings, a number and JSON values with string keys, which JSON always writes",
        )
    }

    /// The problem, as a client reads it.
    pub(crate) fn to_problem(&self) -> Problem {
        Problem {
            kind: self.kind.to_owned(),
            title: self.title.map(str::to_owned),
            status: Some(self.status),
            detail: self.detail.map(str::to_owned),
            instance: None,
        }
    }
}

/// A status as the number of the `status` member.
fn status_code<S: serde::Serializer>(
    status: &StatusCode,
    serializer: S,
) -> Result<S::Ok, S::Error> {
    serializer.serialize_u16(status.as_u16())
}
