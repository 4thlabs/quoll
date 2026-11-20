//! The one error of every route, on the server and on the client.

use std::error::Error as StdError;
use std::fmt;

use http::StatusCode;

use crate::rest::Problem;

#[cfg(feature = "client")]
mod client;

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
///
/// On the client, an answer with an error status is an error with that [status](Error::status), the
/// [problem](Error::problem) when the body is one, and the [body](Error::body) as it came. A call that got no answer
/// to read, a connection refused or an answer that is not the route's result, has no status. Its sources, reqwest's
/// error among them, show the URL of the call by its origin and its path alone: never its credentials, nor its query,
/// where a query scheme sends its key.
///
/// A handler that returns an error its own client received answers 502, or 500 when it is the handler's own mistake,
/// a URL that is not one: the status of another API is not passed on as this one's.
pub struct Error(pub(crate) Box<Repr>);

/// What an error holds, which the server reads to answer it.
pub(crate) enum Repr {
    /// A status and a detail, both sent.
    Problem { status: StatusCode, detail: String },
    /// A failure of the server's own: a 500, whose source is logged, never sent.
    Internal(Box<dyn StdError + Send + Sync>),
    /// What another API answered: its status, its problem when the body is one, and its body as it came. A handler
    /// returning it answers 502, so that the other API's status is not passed on as this one's.
    #[cfg_attr(
        not(any(test, feature = "client")),
        expect(dead_code, reason = "only the client builds it")
    )]
    Upstream {
        status: StatusCode,
        problem: Option<Problem>,
        body: Vec<u8>,
    },
    /// A call to another API that got no answer to read: a client given a URL that is not one, a request that got no
    /// response, or an answer that is not the route's result. It has no status; a handler returning it answers
    /// `answered`, and logs it.
    #[cfg_attr(
        not(any(test, feature = "client")),
        expect(dead_code, reason = "only the client builds it")
    )]
    Failed {
        message: String,
        source: Option<Box<dyn StdError + Send + Sync>>,
        // Where the variant is built but no server answers it.
        #[cfg_attr(
            all(not(feature = "server"), any(test, feature = "client")),
            expect(dead_code, reason = "only the server answers an error")
        )]
        answered: StatusCode,
    },
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
        not(any(test, feature = "client")),
        expect(dead_code, reason = "only the client builds it")
    )]
    pub(crate) fn upstream(status: StatusCode, problem: Option<Problem>, body: Vec<u8>) -> Self {
        Self(Box::new(Repr::Upstream {
            status,
            problem,
            body,
        }))
    }

    /// A call that got no answer to read, which a handler returning it answers with `answered`.
    #[cfg_attr(
        not(any(test, feature = "client")),
        expect(dead_code, reason = "only the client builds it")
    )]
    pub(crate) fn failed(
        message: impl Into<String>,
        source: Option<Box<dyn StdError + Send + Sync>>,
        answered: StatusCode,
    ) -> Self {
        Self(Box::new(Repr::Failed {
            message: message.into(),
            source,
            answered,
        }))
    }

    /// The status of the error: the one given, 500 for an internal error, or the status another API answered.
    ///
    /// A call of the client that got no answer to read has none: a connection refused, a timeout, a middleware's
    /// error, an answer that is not the route's result, or a client given a URL that is not one.
    pub fn status(&self) -> Option<StatusCode> {
        match &*self.0 {
            Repr::Problem { status, .. } | Repr::Upstream { status, .. } => Some(*status),
            Repr::Internal(_) => Some(StatusCode::INTERNAL_SERVER_ERROR),
            Repr::Failed { .. } => None,
        }
    }

    /// A client given what it cannot use, a URL that is not one or a value it cannot write: the mistake of the code
    /// that called it, which a handler returning it answers with a 500.
    #[cfg_attr(
        not(feature = "client"),
        expect(dead_code, reason = "only the client builds it")
    )]
    pub(crate) fn misuse(
        message: impl Into<String>,
        source: Option<Box<dyn StdError + Send + Sync>>,
    ) -> Self {
        Self::failed(message, source, StatusCode::INTERNAL_SERVER_ERROR)
    }

    /// The RFC 9457 problem another API answered, when its body is one: its content type is
    /// `application/problem+json` and it holds a JSON object.
    pub fn problem(&self) -> Option<&Problem> {
        match &*self.0 {
            Repr::Upstream { problem, .. } => problem.as_ref(),
            Repr::Problem { .. } | Repr::Internal(_) | Repr::Failed { .. } => None,
        }
    }

    /// The body another API answered with an error status, as it came: its first 64 KiB, as the client reads no
    /// further. `None` for an error that is not such an answer.
    pub fn body(&self) -> Option<&[u8]> {
        match &*self.0 {
            Repr::Upstream { body, .. } => Some(body),
            Repr::Problem { .. } | Repr::Internal(_) | Repr::Failed { .. } => None,
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
            Repr::Upstream {
                status,
                problem,
                body,
            } => f
                .debug_struct("Upstream")
                .field("status", status)
                .field("problem", problem)
                .field("body", &String::from_utf8_lossy(body))
                .finish(),
            Repr::Failed {
                message, source, ..
            } => f
                .debug_struct("Failed")
                .field("message", message)
                .field("source", source)
                .finish(),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &*self.0 {
            Repr::Problem { status, detail } => write!(f, "{detail} ({})", status.as_u16()),
            Repr::Internal(_) => f.write_str("internal error"),
            Repr::Upstream {
                status, problem, ..
            } => {
                write!(f, "another api answered {}", status.as_u16())?;

                match problem.as_ref().and_then(Problem::detail) {
                    Some(detail) => write!(f, ": {detail}"),
                    None => Ok(()),
                }
            }
            Repr::Failed { message, .. } => f.write_str(message),
        }
    }
}

impl StdError for Error {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match &*self.0 {
            Repr::Internal(source)
            | Repr::Failed {
                source: Some(source),
                ..
            } => Some(&**source),
            Repr::Problem { .. } | Repr::Upstream { .. } | Repr::Failed { source: None, .. } => {
                None
            }
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
                Error::upstream(StatusCode::NOT_FOUND, None, Vec::new()),
                StatusCode::NOT_FOUND,
            ),
        ];

        for (error, status) in cases {
            assert_eq!(error.status(), Some(status), "{error:?}");
        }
    }

    #[test]
    fn a_call_that_got_no_answer_has_no_status_and_keeps_its_source() {
        let error = Error::failed(
            "the call `notes_get` got no response",
            Some("connection refused".into()),
            StatusCode::BAD_GATEWAY,
        );

        assert_eq!(error.status(), None);
        assert_eq!(error.to_string(), "the call `notes_get` got no response");
        assert_eq!(
            error.source().map(ToString::to_string).as_deref(),
            Some("connection refused")
        );
        assert!(error.problem().is_none() && error.body().is_none());
    }

    #[test]
    fn an_answer_of_another_api_says_its_status_and_its_detail() {
        let problem = Problem {
            kind: "about:blank".into(),
            title: None,
            status: Some(StatusCode::NOT_FOUND),
            detail: Some("no such note".into()),
            instance: None,
        };
        let error = Error::upstream(StatusCode::NOT_FOUND, Some(problem), b"{}".to_vec());

        assert_eq!(error.to_string(), "another api answered 404: no such note");
        assert_eq!(error.body(), Some(&b"{}"[..]));
        assert_eq!(
            Error::upstream(StatusCode::BAD_GATEWAY, None, Vec::new()).to_string(),
            "another api answered 502"
        );
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
