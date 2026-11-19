//! The one error of every route, on the server and on the client.

use std::error::Error as StdError;
use std::fmt;

use http::StatusCode;

/// A result whose error is [`Error`], what every route returns.
pub type Result<T, E = Error> = std::result::Result<T, E>;

/// The error of a route: what a handler answers, and what a client receives.
///
/// A handler builds one with a status and a detail, which the server answers as an RFC 9457 problem in JSON:
///
/// ```
/// use quoll::rest::{Error, Result};
///
/// fn note(id: i64) -> Result<String> {
///     if id == 1 {
///         return Ok("Milk".into());
///     }
///
///     Err(Error::not_found("no such note"))
/// }
///
/// assert_eq!(note(2).unwrap_err().status(), Some(http::StatusCode::NOT_FOUND));
/// ```
///
/// A failure of the server's own, a database that does not answer, is [`Error::internal`]: it is logged, and the
/// caller sees only a 500.
pub struct Error(pub(crate) Box<Repr>);

/// What an error holds, which the server reads to answer it.
pub(crate) enum Repr {
    /// A status and a detail, both sent.
    Problem { status: StatusCode, detail: String },
    /// A failure of the server's own: a 500, whose source is logged, never sent.
    Internal(Box<dyn StdError + Send + Sync>),
    /// What another API answered: its status, and its body as it came. A handler returning it answers 502, so that the
    /// other API's status is not passed on as this one's.
    #[cfg_attr(
        not(test),
        expect(dead_code, reason = "the client builds it, in a later change")
    )]
    Upstream { status: StatusCode, body: Vec<u8> },
}

impl Error {
    /// An error with this status and this detail, both sent to the caller.
    ///
    /// The status is a client or a server error, `4xx` or `5xx`: any other is answered as a 500, and logged.
    pub fn new(status: StatusCode, detail: impl Into<String>) -> Self {
        Self(Box::new(Repr::Problem {
            status,
            detail: detail.into(),
        }))
    }

    /// A 404: what the route names does not exist.
    pub fn not_found(detail: impl Into<String>) -> Self {
        Self::new(StatusCode::NOT_FOUND, detail)
    }

    /// A 400: the request cannot be served as it is.
    pub fn bad_request(detail: impl Into<String>) -> Self {
        Self::new(StatusCode::BAD_REQUEST, detail)
    }

    /// A 401: the request has no valid credential.
    pub fn unauthorized(detail: impl Into<String>) -> Self {
        Self::new(StatusCode::UNAUTHORIZED, detail)
    }

    /// A 500 for a failure of the server's own. The server logs `error` through `tracing` and never sends it: the
    /// caller sees the status alone.
    ///
    /// ```
    /// use quoll::rest::Error;
    ///
    /// let error = "x".parse::<u8>().map_err(Error::internal);
    ///
    /// assert_eq!(error.unwrap_err().status(), Some(http::StatusCode::INTERNAL_SERVER_ERROR));
    /// ```
    pub fn internal(error: impl Into<Box<dyn StdError + Send + Sync>>) -> Self {
        Self(Box::new(Repr::Internal(error.into())))
    }

    /// What another API answered with an error status, kept as it came.
    #[cfg_attr(
        not(test),
        expect(dead_code, reason = "the client builds it, in a later change")
    )]
    pub(crate) fn upstream(status: StatusCode, body: Vec<u8>) -> Self {
        Self(Box::new(Repr::Upstream { status, body }))
    }

    /// The status of the error: the one given, 500 for an internal error, or the status another API answered.
    ///
    /// It is `Some` for every error built today; a failure with no response at all, as a client meets one, has none.
    pub fn status(&self) -> Option<StatusCode> {
        match &*self.0 {
            Repr::Problem { status, .. } | Repr::Upstream { status, .. } => Some(*status),
            Repr::Internal(_) => Some(StatusCode::INTERNAL_SERVER_ERROR),
        }
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &*self.0 {
            Repr::Problem { status, detail } => f
                .debug_struct("Error")
                .field("status", status)
                .field("detail", detail)
                .finish(),
            Repr::Internal(source) => f.debug_tuple("Internal").field(source).finish(),
            Repr::Upstream { status, body } => f
                .debug_struct("Upstream")
                .field("status", status)
                .field("body", &String::from_utf8_lossy(body))
                .finish(),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &*self.0 {
            Repr::Problem { status, detail } => write!(f, "{detail} ({})", status.as_u16()),
            Repr::Internal(_) => f.write_str("internal error"),
            Repr::Upstream { status, .. } => {
                write!(f, "another api answered {}", status.as_u16())
            }
        }
    }
}

impl StdError for Error {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match &*self.0 {
            Repr::Internal(source) => Some(&**source),
            Repr::Problem { .. } | Repr::Upstream { .. } => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn each_constructor_gives_its_status() {
        let cases = [
            (Error::not_found("x"), StatusCode::NOT_FOUND),
            (Error::bad_request("x"), StatusCode::BAD_REQUEST),
            (Error::unauthorized("x"), StatusCode::UNAUTHORIZED),
            (Error::new(StatusCode::CONFLICT, "x"), StatusCode::CONFLICT),
            (Error::internal("x"), StatusCode::INTERNAL_SERVER_ERROR),
            (
                Error::upstream(StatusCode::NOT_FOUND, Vec::new()),
                StatusCode::NOT_FOUND,
            ),
        ];

        for (error, status) in cases {
            assert_eq!(error.status(), Some(status), "{error:?}");
        }
    }

    #[test]
    fn an_internal_error_keeps_its_source_out_of_its_message() {
        let error = Error::internal("the database is down");

        assert_eq!(error.to_string(), "internal error");
        assert_eq!(
            error.source().map(ToString::to_string).as_deref(),
            Some("the database is down")
        );
    }

    #[test]
    fn a_problem_says_its_detail_and_its_status() {
        assert_eq!(
            Error::not_found("no such note").to_string(),
            "no such note (404)"
        );
    }
}
