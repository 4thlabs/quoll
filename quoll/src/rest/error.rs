//! The error of every route, on the server and on the client, generic over the shape of the errors an api answers.

use std::error::Error as StdError;
use std::fmt;

use http::{HeaderMap, StatusCode, Version};

use crate::rest::{Problem, Response};

#[cfg(feature = "client")]
mod client;

/// A result whose error is an [`Error`] of the shape `E`: what every route returns, `rest::Result<Note>` of the
/// default shape, an RFC 9457 [`Problem`], and `rest::Result<Repository, GitHubError>` on the client of an api that
/// says `#[error(GitHubError)]`.
pub type Result<T, E = Problem> = std::result::Result<T, Error<E>>;

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
/// On the client, an answer with an error status is an error with that [status](Error::status), its
/// [detail](Error::detail) when the body is one of the api's error shape, `E`, and the [body](Error::body) as it came.
/// `E` is an RFC 9457 [`Problem`] unless the contract says otherwise, `#[error(GitHubError)]` on its api, a group or
/// a route, for another API that answers its errors in a shape of its own: a type that implements `Deserialize`,
/// `Display` and `Debug`, whose `Display` gives the error's message, `another api answered 401: Bad credentials`. A
/// body that is not an `E`, a page of HTML from a proxy, gives no detail, and keeps its status and its body.
///
/// A call that got no answer to read, a connection refused or an answer that is not the route's result, has no
/// status. Its sources show the URL of the call by its origin and its path alone, without its credentials and its
/// query, where a query scheme sends its key: the errors of reqwest and of reqwest-middleware, as the client and its
/// middleware give them, are stripped. A reqwest error that a middleware of the application's own wraps in an error
/// type of its own is out of reach: the middleware strips it, with `reqwest::Error::without_url`.
///
/// A handler that returns an error its own client received answers 502, or 500 when it is the handler's own mistake,
/// a URL that is not one: the status of another API is not passed on as this one's. An error of another shape goes up
/// into a handler's `rest::Error` with `?`, which keeps its status, its message and its body, and leaves its detail.
pub struct Error<E = Problem>(pub(crate) Box<Repr<E>>);

/// What an error holds, which the server reads to answer it.
pub(crate) enum Repr<E> {
    /// A status and a detail, both sent.
    Problem { status: StatusCode, detail: String },
    /// A failure of the server's own: a 500, whose source is logged, never sent.
    Internal(Box<dyn StdError + Send + Sync>),
    /// What another API answered: its status, the answer with its detail when the body is an `E`, its body as it
    /// came, none for an error a fake builds, and its message, which the detail gives. A handler returning it answers
    /// 502, so that the other API's status is not passed on as this one's.
    Upstream {
        status: StatusCode,
        answer: Option<Response<E>>,
        body: Option<Vec<u8>>,
        message: Option<String>,
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
    /// With the `client` feature, the URL of a reqwest error given as `error`, alone or as the error of a
    /// reqwest-middleware chain, keeps its origin and its path alone, as the client's own errors do: the log shows no
    /// credentials and no query, where a query scheme sends its key. A reqwest error inside an error type of the
    /// application's own is the application's to strip, with `reqwest::Error::without_url`.
    ///
    /// ```
    /// use quoll::rest::Error;
    ///
    /// let error = "x".parse::<u8>().map_err(Error::internal);
    ///
    /// assert_eq!(error.unwrap_err().status(), Some(http::StatusCode::INTERNAL_SERVER_ERROR));
    /// ```
    pub fn internal(error: impl Into<Box<dyn StdError + Send + Sync>>) -> Self {
        let error = error.into();
        #[cfg(feature = "client")]
        let error = client::without_query(error);

        Self(Box::new(Repr::Internal(error)))
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
}

impl<E> Error<E> {
    /// What another API answered with an error status, `detail`, as a fake of a group's client gives it: an error with
    /// `status`, whose [`response`](Error::response) has no headers, whose `Display` says the message of `detail`, and
    /// that has no [`body`](Error::body), as nothing came. It is for the shape of an api with `#[error(E)]`: a fake of
    /// an api of problems returns [`Error::not_found`] and the others, with their status.
    ///
    /// ```
    /// use std::fmt;
    ///
    /// use http::StatusCode;
    /// use quoll::rest;
    /// use serde::Deserialize;
    ///
    /// #[derive(Debug, Deserialize)]
    /// struct GitHubError {
    ///     message: String,
    /// }
    ///
    /// impl fmt::Display for GitHubError {
    ///     fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    ///         f.write_str(&self.message)
    ///     }
    /// }
    ///
    /// let error = rest::Error::answered(StatusCode::NOT_FOUND, GitHubError { message: "Not Found".into() });
    ///
    /// assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    /// assert_eq!(error.detail().map(|detail| detail.message.as_str()), Some("Not Found"));
    /// assert_eq!(error.to_string(), "another api answered 404: Not Found");
    /// ```
    pub fn answered(status: StatusCode, detail: E) -> Self
    where
        E: fmt::Display,
    {
        let message = detail.to_string();
        let answer = Response::from_parts(status, Version::HTTP_11, HeaderMap::new(), detail);

        Self::upstream(status, Some(answer), None, Some(message))
    }

    /// What another API answered with an error status: the answer with its detail when its body is an `E`, its body
    /// as it came, and the message of its detail.
    pub(crate) fn upstream(
        status: StatusCode,
        answer: Option<Response<E>>,
        body: Option<Vec<u8>>,
        message: Option<String>,
    ) -> Self {
        Self(Box::new(Repr::Upstream {
            status,
            answer,
            body,
            message,
        }))
    }

    /// The error, as an error of the shape `F`: a call that got no answer as it is, and an answer, whose detail is
    /// left, with its status, its body and its message.
    pub(crate) fn cast<F>(self) -> Error<F> {
        Error(Box::new(match *self.0 {
            Repr::Problem { status, detail } => Repr::Problem { status, detail },
            Repr::Internal(source) => Repr::Internal(source),
            Repr::Upstream {
                status,
                body,
                message,
                ..
            } => Repr::Upstream {
                status,
                answer: None,
                body,
                message,
            },
            Repr::Failed {
                message,
                source,
                answered,
            } => Repr::Failed {
                message,
                source,
                answered,
            },
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

    /// What another API answered, when its body is one of the api's error shape, `E`: an RFC 9457 [`Problem`] of
    /// the content type `application/problem+json` by default, or the JSON of the `E` the contract says with
    /// `#[error(E)]`.
    ///
    /// `None` for an error that is not such an answer, or an answer whose body is not an `E`: a page of HTML from a
    /// proxy, an empty body, a 304, or a body longer than the 64 KiB the client keeps.
    pub fn detail(&self) -> Option<&E> {
        self.response().map(Response::body)
    }

    /// The answer another API gave, when its body is an `E`: its status, its version, its headers, a rate limit's or
    /// a `Retry-After`, and its [detail](Error::detail).
    ///
    /// `None` when [`detail`](Error::detail) is: the headers of any answer are those of the call's `.response()`. The
    /// error of an `event: problem` in a stream, or one a fake builds, has a response of its status alone, in HTTP/1.1
    /// and with no headers.
    pub fn response(&self) -> Option<&Response<E>> {
        match &*self.0 {
            Repr::Upstream { answer, .. } => answer.as_ref(),
            Repr::Problem { .. } | Repr::Internal(_) | Repr::Failed { .. } => None,
        }
    }

    /// The body another API answered with an error status, as it came: its first 64 KiB, as the client reads no
    /// further. `None` for an error that is not such an answer, or one a fake built with
    /// [`answered`](Error::answered).
    pub fn body(&self) -> Option<&[u8]> {
        match &*self.0 {
            Repr::Upstream { body, .. } => body.as_deref(),
            Repr::Problem { .. } | Repr::Internal(_) | Repr::Failed { .. } => None,
        }
    }
}

/// An error of another shape, carried up into a `rest::Error`, a handler's: its status, its message and its body
/// kept, its detail left, as a `rest::Error` holds a problem alone. A handler returning it answers as the error it
/// comes from: 502 for an answer of another API.
impl<E> From<Error<E>> for Error
where
    E: serde::de::DeserializeOwned + fmt::Display + fmt::Debug + Send + Sync + 'static,
{
    fn from(error: Error<E>) -> Self {
        error.cast()
    }
}

impl<E: fmt::Debug> fmt::Debug for Error<E> {
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
                answer,
                body,
                ..
            } => f
                .debug_struct("Upstream")
                .field("status", status)
                .field("detail", &answer.as_ref().map(Response::body))
                .field("body", &body.as_deref().map(String::from_utf8_lossy))
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

impl<E> fmt::Display for Error<E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &*self.0 {
            Repr::Problem { status, detail } => write!(f, "{detail} ({})", status.as_u16()),
            Repr::Internal(_) => f.write_str("internal error"),
            Repr::Upstream {
                status, message, ..
            } => {
                write!(f, "another api answered {}", status.as_u16())?;

                match message {
                    Some(message) => write!(f, ": {message}"),
                    None => Ok(()),
                }
            }
            Repr::Failed { message, .. } => f.write_str(message),
        }
    }
}

impl<E: fmt::Debug> StdError for Error<E> {
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
                Error::upstream(StatusCode::NOT_FOUND, None, Some(Vec::new()), None),
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
        assert!(error.detail().is_none() && error.body().is_none());
    }

    #[test]
    fn an_answer_of_another_api_says_its_status_and_its_message() {
        let error = Error::<Problem>::upstream(
            StatusCode::NOT_FOUND,
            None,
            Some(b"{}".to_vec()),
            Some("no such note".into()),
        );

        assert_eq!(error.to_string(), "another api answered 404: no such note");
        assert_eq!(error.body(), Some(&b"{}"[..]));
        assert_eq!(
            Error::<Problem>::upstream(StatusCode::BAD_GATEWAY, None, Some(Vec::new()), None)
                .to_string(),
            "another api answered 502"
        );
    }

    /// An error shape of another API's, as GitHub's.
    #[derive(Debug, PartialEq, Eq, serde::Deserialize)]
    struct Shape {
        message: String,
    }

    impl fmt::Display for Shape {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str(&self.message)
        }
    }

    #[test]
    fn an_answer_a_fake_builds_has_its_detail_and_no_body() {
        let error = Error::answered(
            StatusCode::UNAUTHORIZED,
            Shape {
                message: "Bad credentials".into(),
            },
        );
        let response = error.response().expect("an answer");

        assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
        assert_eq!(response.status(), StatusCode::UNAUTHORIZED);
        assert!(response.headers().is_empty());
        assert_eq!(
            error.detail().map(|shape| shape.message.as_str()),
            Some("Bad credentials")
        );
        assert_eq!(error.body(), None);
        assert_eq!(
            error.to_string(),
            "another api answered 401: Bad credentials"
        );
    }

    #[test]
    fn an_error_of_another_shape_goes_up_with_its_status_its_message_and_its_body() {
        let answer = Response::from_parts(
            StatusCode::FORBIDDEN,
            Version::HTTP_11,
            HeaderMap::new(),
            Shape {
                message: "rate limited".into(),
            },
        );
        let error = Error::upstream(
            StatusCode::FORBIDDEN,
            Some(answer),
            Some(br#"{"message":"rate limited"}"#.to_vec()),
            Some("rate limited".into()),
        );

        let carried: Error = error.into();

        assert_eq!(carried.status(), Some(StatusCode::FORBIDDEN));
        assert_eq!(
            carried.to_string(),
            "another api answered 403: rate limited"
        );
        assert_eq!(carried.body(), Some(&br#"{"message":"rate limited"}"#[..]));
        assert!(carried.detail().is_none(), "a shape that is not a problem");

        let failed: Error = Error::failed("got no response", None, StatusCode::BAD_GATEWAY)
            .cast::<Shape>()
            .into();

        assert_eq!(failed.status(), None);
        assert_eq!(failed.to_string(), "got no response");
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
