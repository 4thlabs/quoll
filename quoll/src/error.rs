//! The error of every route, on the server and on the client, generic over the shape of the errors an api answers.

use std::error::Error as StdError;
use std::fmt;
use std::time::Duration;

use http::{HeaderMap, HeaderName, HeaderValue, StatusCode, Version};

use crate::{Problem, Response};

#[cfg(feature = "client")]
mod client;
mod headers;
mod kind;

use headers::Headers;

pub use kind::ErrorKind;

/// A result whose error is an [`Error`] of the shape `E`: what every route returns, `quoll::Result<Note>` of the
/// default shape, an RFC 9457 [`Problem`], and `quoll::Result<Repository, GitHubError>` on the client of an api that
/// says `#[error(GitHubError)]`.
pub type Result<T, E = Problem> = std::result::Result<T, Error<E>>;

/// The error of a route: what a handler answers, and what a client receives.
///
/// A handler builds one with a status and a detail, which the server answers as an RFC 9457 problem in JSON:
///
/// ```
/// use quoll::{Error, Result};
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
/// An error carries headers, which the server sends with its status: a `Retry-After` on a 429 with
/// [`retry_after`](Error::retry_after), any other with [`with_header`](Error::with_header). The client reads those of
/// an answer with [`headers`](Error::headers).
///
/// On the client, an answer with an error status is an error with that [status](Error::status), its
/// [detail](Error::detail) when the body is one of the api's error shape, `E`, and the [body](Error::body) as it came.
/// `E` is an RFC 9457 [`Problem`] unless the contract says otherwise, `#[error(GitHubError)]` on its api, a group or
/// a route, for another API that answers its errors in a shape of its own: a type that implements `Deserialize`,
/// `Display` and `Debug`, whose `Display` gives the error's message, `another api answered 401: Bad credentials`. A
/// body that is not an `E`, a page of HTML from a proxy, gives no detail, and keeps its status and its body.
///
/// Every error has a [kind](Error::kind), an [`ErrorKind`]: [`ErrorKind::Status`] of its status, or what went wrong
/// in a call that got no answer to read, which has no status: a connection refused is [`ErrorKind::Connect`], an
/// answer that is not the route's result [`ErrorKind::Decode`]. Its sources show the URL of the call by its origin and
/// its path alone, without its credentials and its query, where a query scheme sends its key: the errors of reqwest
/// and of reqwest-middleware, as the client and its middleware give them, are stripped. A reqwest error that a
/// middleware of the application's, or of another crate, wraps in an error type of its own, as reqwest-retry's
/// `RetryError`, is out of reach, in the error and in the span of [`client::trace`]: the middleware strips it, with
/// `reqwest::Error::without_url`, or [`client::retry`] takes the place of reqwest-retry's. Such an error is of the kind
/// [`ErrorKind::Middleware`], its cause among its sources.
///
#[cfg_attr(feature = "client", doc = "[`client::trace`]: crate::client::trace")]
#[cfg_attr(feature = "client", doc = "[`client::retry`]: crate::client::retry")]
#[cfg_attr(not(feature = "client"), doc = "[`client::trace`]: crate#features")]
#[cfg_attr(not(feature = "client"), doc = "[`client::retry`]: crate#features")]
///
/// A handler that returns an error its own client received answers 502, or 500 when it is the handler's own mistake,
/// a URL that is not one: the status of another API is not passed on as this one's. An error of another shape goes up
/// into a handler's `quoll::Error` with `?`, which keeps its status, its message and its body, and leaves its detail.
pub struct Error<E = Problem>(pub(crate) Box<Repr<E>>, Headers);

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
    /// A call to another API that got no answer to read, of `kind`, never a `Status`: a client given a URL that is not
    /// one, a request that got no response, a middleware's error, or an answer that is not the route's result. It has
    /// no status; a handler returning it answers 500 for an [`ErrorKind::Invalid`], 502 for any other, and logs it.
    Failed {
        kind: ErrorKind,
        message: String,
        source: Option<Box<dyn StdError + Send + Sync>>,
    },
}

impl Error {
    /// An error with this status and this detail, both sent to the caller.
    ///
    /// The status is a client or a server error, `4xx` or `5xx`: any other is answered as a 500, and logged.
    pub fn new(status: StatusCode, detail: impl Into<String>) -> Self {
        Self::of(Repr::Problem {
            status,
            detail: detail.into(),
        })
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
    /// use quoll::Error;
    ///
    /// let error = "x".parse::<u8>().map_err(Error::internal);
    ///
    /// assert_eq!(error.unwrap_err().status(), Some(http::StatusCode::INTERNAL_SERVER_ERROR));
    /// ```
    pub fn internal(error: impl Into<Box<dyn StdError + Send + Sync>>) -> Self {
        let error = error.into();
        #[cfg(feature = "client")]
        let error = client::without_query(error);

        Self::of(Repr::Internal(error))
    }

    /// The error, as an error of the shape `F`: an error of a status, an internal one and a call that got no answer
    /// as they are, and the answer of another API, whose problem is left, with its status, its body and its message.
    /// Every error keeps its [headers](Error::headers).
    ///
    /// A fake of an api with `#[error(F)]` so returns an error of a status alone, which has no detail, as an answer
    /// whose body is not an `F`:
    ///
    /// ```
    /// use std::fmt;
    ///
    /// use http::StatusCode;
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
    /// let error: quoll::Error<GitHubError> = quoll::Error::not_found("no such repository").into_shape();
    ///
    /// assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    /// assert!(error.detail().is_none());
    ///
    /// let failed: quoll::Error<GitHubError> = quoll::Error::internal("the fake is down").into_shape();
    ///
    /// assert_eq!(failed.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
    /// ```
    pub fn into_shape<F>(self) -> Error<F> {
        self.cast()
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
        Self::failed_from(ErrorKind::Invalid, message, source)
    }
}

impl<E> Error<E> {
    /// A call of the client that failed with `kind`, and says `message`: what a fake of a group's client returns, as
    /// the real client fails. Its [`kind`](Error::kind) is `kind`, and its `.response()` is as the real client's.
    ///
    /// - Of any kind but [`ErrorKind::Status`], it is a call that got no answer to read: it has no
    ///   [status](Error::status), its `.response()` is this error, and a handler returning it answers 502, or 500
    ///   for an [`ErrorKind::Invalid`].
    /// - Of `ErrorKind::Status(status)`, it is what another API answered with `status`, whose body is not the api's
    ///   error shape: it has that status and no [detail](Error::detail), its `Display` is
    ///   `another api answered 404: message`, its `.response()` an answer of that status, and a handler returning it
    ///   answers 502. An answer with a detail is [`Error::answered`].
    ///
    /// ```
    /// use http::StatusCode;
    /// use quoll::{Error, ErrorKind};
    ///
    /// let error: Error = Error::failed(ErrorKind::Decode, "an item that does not read as a note");
    ///
    /// assert_eq!(error.status(), None);
    /// assert_eq!(error.kind(), ErrorKind::Decode);
    /// assert_eq!(error.to_string(), "an item that does not read as a note");
    ///
    /// let proxy: Error = Error::failed(ErrorKind::Status(StatusCode::BAD_GATEWAY), "a page of nginx");
    ///
    /// assert_eq!(proxy.status(), Some(StatusCode::BAD_GATEWAY));
    /// assert!(proxy.detail().is_none());
    /// assert_eq!(proxy.to_string(), "another api answered 502: a page of nginx");
    /// ```
    pub fn failed(kind: ErrorKind, message: impl Into<String>) -> Self {
        if let ErrorKind::Status(status) = kind {
            let message = Some(message.into()).filter(|message| !message.is_empty());

            return Self::upstream(status, None, None, message);
        }

        Self::failed_from(kind, message, None)
    }

    /// A call that got no answer to read, of `kind`, which is never a `Status`, with the error it comes from: the
    /// client's classification never gives one, and an answer is [`Error::upstream`].
    pub(crate) fn failed_from(
        kind: ErrorKind,
        message: impl Into<String>,
        source: Option<Box<dyn StdError + Send + Sync>>,
    ) -> Self {
        debug_assert!(
            !matches!(kind, ErrorKind::Status(_)),
            "a call that got no answer has no status"
        );

        Self::of(Repr::Failed {
            kind,
            message: message.into(),
            source,
        })
    }

    /// What another API answered with an error status, `detail`, as a fake of a group's client gives it: an error with
    /// `status`, whose [`response`](Error::response) has no headers until it is given some with
    /// [`with_header`](Error::with_header), whose `Display` says the message of `detail`, and
    /// that has no [`body`](Error::body), as nothing came. It is for the shape of an api with `#[error(E)]`: a fake of
    /// an api of problems returns [`Error::not_found`] and the others, with their status, and a fake of another shape
    /// gives them its shape with [`into_shape`](Error::into_shape).
    ///
    /// ```
    /// use std::fmt;
    ///
    /// use http::StatusCode;
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
    /// let error = quoll::Error::answered(StatusCode::NOT_FOUND, GitHubError { message: "Not Found".into() });
    ///
    /// assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    /// assert_eq!(error.detail().map(|detail| detail.message.as_str()), Some("Not Found"));
    /// assert_eq!(error.to_string(), "another api answered 404: Not Found");
    /// ```
    pub fn answered(status: StatusCode, detail: E) -> Self
    where
        E: fmt::Display,
    {
        let message = Some(detail.to_string()).filter(|message| !message.is_empty());
        let answer = Response::from_parts(status, Version::HTTP_11, HeaderMap::new(), detail);

        Self::upstream(status, Some(answer), None, message)
    }

    /// What another API answered with an error status: the answer with its detail when its body is an `E`, its body
    /// as it came, and the message of its detail.
    pub(crate) fn upstream(
        status: StatusCode,
        answer: Option<Response<E>>,
        body: Option<Vec<u8>>,
        message: Option<String>,
    ) -> Self {
        Self::of(Repr::Upstream {
            status,
            answer,
            body,
            message,
        })
    }

    /// The error with the headers of the answer the client received, `headers`, which its `answer` holds too when it
    /// has one.
    #[cfg_attr(
        not(feature = "client"),
        expect(dead_code, reason = "only the client receives an answer")
    )]
    pub(crate) fn received(mut self, headers: HeaderMap) -> Self {
        self.1 = Headers::received(headers);
        self
    }

    /// An error of `repr`, with no headers.
    fn of(repr: Repr<E>) -> Self {
        Self(Box::new(repr), Headers::default())
    }

    /// The error, as an error of the shape `F`: an error of a status, an internal one and a call that got no answer
    /// as they are, and an answer, whose detail is left, with its status, its body and its message; and its headers.
    pub(crate) fn cast<F>(self) -> Error<F> {
        let Self(repr, headers) = self;
        let repr = Box::new(match *repr {
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
                kind,
                message,
                source,
            } => Repr::Failed {
                kind,
                message,
                source,
            },
        });

        Error(repr, headers)
    }

    /// The status of the error, `Some(status)` exactly when its [kind](Error::kind) is
    /// `ErrorKind::Status(status)`: the one given, 500 for an internal error, or the status another API answered.
    ///
    /// A call of the client that got no answer to read has none: its kind says what went wrong, a connection
    /// refused, [`ErrorKind::Connect`], a deadline passed, [`ErrorKind::Timeout`], an answer that is not the route's
    /// result, [`ErrorKind::Decode`]...
    pub fn status(&self) -> Option<StatusCode> {
        match self.kind() {
            ErrorKind::Status(status) => Some(status),
            _ => None,
        }
    }

    /// What the error is: [`ErrorKind::Status`] of its status, the one a handler gave, 500 for an internal error, or
    /// the one another API answered, its [detail](Error::detail) beside; or what went wrong in a call of the client
    /// that got no answer to read, [`ErrorKind::Connect`], [`ErrorKind::Timeout`], [`ErrorKind::Decode`]...
    ///
    /// ```
    /// use http::StatusCode;
    /// use quoll::{Error, ErrorKind};
    ///
    /// let refused: Error = Error::failed(ErrorKind::Connect, "refused");
    ///
    /// assert_eq!(Error::not_found("no such note").kind(), ErrorKind::Status(StatusCode::NOT_FOUND));
    /// assert_eq!(Error::internal("down").kind(), ErrorKind::Status(StatusCode::INTERNAL_SERVER_ERROR));
    /// assert_eq!(refused.kind(), ErrorKind::Connect);
    /// ```
    pub fn kind(&self) -> ErrorKind {
        match &*self.0 {
            Repr::Problem { status, .. } | Repr::Upstream { status, .. } => {
                ErrorKind::Status(*status)
            }
            Repr::Internal(_) => ErrorKind::Status(StatusCode::INTERNAL_SERVER_ERROR),
            Repr::Failed { kind, .. } => *kind,
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
    /// `None` when [`detail`](Error::detail) is: the headers of any answer are those of the call's `.response()`, and
    /// the error's [`headers`](Error::headers). The error of an `event: problem` in a stream, or one a fake builds,
    /// has a response of its status alone, in HTTP/1.1 and with the headers given with
    /// [`with_header`](Error::with_header) alone.
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

    /// The error with the header `name` of `value`, after those of that name it has already: a `WWW-Authenticate` on
    /// a 401, an `Allow` on a 405, a `Retry-After` on a 429 or a 503, which [`retry_after`](Error::retry_after)
    /// writes from a `Duration`.
    ///
    /// The server answers them with the error's status and its problem, and the client reads them back with
    /// [`headers`](Error::headers). An error the server answers with a status of its own, 500 for an internal error,
    /// 502 for an error its client received, is answered with that status alone, without them. A `WWW-Authenticate`
    /// on a 401 takes the place of the challenges of the route's schemes. An error after the 200 of a stream, an
    /// `event: problem`, has no headers to send.
    ///
    /// `name` is a [`HeaderName`], as `http::header::WWW_AUTHENTICATE`, or its text; `value` a [`HeaderValue`], a
    /// string or a number. A name or a value that is not one is kept out of the headers, and so is a header that
    /// belongs to a connection, `Connection`, `Keep-Alive`, `Proxy-Connection`, `TE`, `Trailer`,
    /// `Transfer-Encoding` or `Upgrade`, or that quoll writes from the problem, `Content-Type` or `Content-Length`:
    /// the server then answers the error with a 500, logged with what was refused, without its value, as it answers
    /// a [`Response::header`] that cannot be sent.
    ///
    /// ```
    /// use http::StatusCode;
    /// use http::header::WWW_AUTHENTICATE;
    /// use quoll::Error;
    ///
    /// let error = Error::unauthorized("the token has expired")
    ///     .with_header(WWW_AUTHENTICATE, r#"Bearer error="invalid_token""#);
    ///
    /// assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
    /// assert_eq!(error.headers()[WWW_AUTHENTICATE], r#"Bearer error="invalid_token""#);
    /// ```
    #[must_use]
    pub fn with_header<K, V>(mut self, name: K, value: V) -> Self
    where
        K: TryInto<HeaderName>,
        V: TryInto<HeaderValue>,
    {
        let added = self.1.append(name, value);

        // The answer a fake builds keeps the headers of its error, as the client's does.
        if let (
            Some((name, value)),
            Repr::Upstream {
                answer: Some(answer),
                ..
            },
        ) = (added, &mut *self.0)
        {
            answer.headers_mut().append(name, value);
        }

        self
    }

    /// The error with a `Retry-After` of `wait`, in place of any it has: its seconds, a part of one counted as a
    /// whole, so that a client does not come back before `wait` has passed. A 429 or a 503 says so when to try again,
    /// which [`client::retry`] reads with [`Attempt::retry_after`].
    ///
    /// ```
    /// use std::time::Duration;
    ///
    /// use http::StatusCode;
    /// use http::header::RETRY_AFTER;
    /// use quoll::Error;
    ///
    /// let error = Error::new(StatusCode::TOO_MANY_REQUESTS, "too many failed sign-ins")
    ///     .retry_after(Duration::from_secs(30));
    ///
    /// assert_eq!(error.headers()[RETRY_AFTER], "30");
    /// ```
    ///
    #[cfg_attr(feature = "client", doc = "[`client::retry`]: crate::client::retry")]
    #[cfg_attr(
        feature = "client",
        doc = "[`Attempt::retry_after`]: crate::client::Attempt::retry_after"
    )]
    #[cfg_attr(not(feature = "client"), doc = "[`client::retry`]: crate#features")]
    #[cfg_attr(
        not(feature = "client"),
        doc = "[`Attempt::retry_after`]: crate#features"
    )]
    #[must_use]
    pub fn retry_after(mut self, wait: Duration) -> Self {
        let value = self.1.retry_after(wait);

        if let Repr::Upstream {
            answer: Some(answer),
            ..
        } = &mut *self.0
        {
            answer
                .headers_mut()
                .insert(http::header::RETRY_AFTER, value);
        }

        self
    }

    /// The headers of the error: those given with [`with_header`](Error::with_header) and
    /// [`retry_after`](Error::retry_after), and, for an answer the client received, the headers of that answer,
    /// whatever its body, before those given. Empty for any other error.
    ///
    /// On the client, a `Retry-After`, a `WWW-Authenticate` or a rate limit's headers are read here, of an answer
    /// whose body is not the api's shape too, which has no [`response`](Error::response).
    pub fn headers(&self) -> &HeaderMap {
        self.1.map()
    }

    /// The first header given to [`with_header`](Error::with_header) that was refused, which says why: the server
    /// answers the error with a 500.
    #[cfg_attr(
        not(feature = "server"),
        expect(dead_code, reason = "only the server answers an error")
    )]
    pub(crate) fn refused_header(&self) -> Option<&str> {
        self.1.refused()
    }
}

/// An error of another shape, carried up into a `quoll::Error`, a handler's: its status, its message, its body and its
/// headers kept, its detail left, as a `quoll::Error` holds a problem alone. A handler returning it answers as the
/// error it comes from: 502 for an answer of another API, without its headers.
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
                kind,
                message,
                source,
            } => f
                .debug_struct("Failed")
                .field("kind", kind)
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
        let error = Error::<Problem>::failed_from(
            ErrorKind::Connect,
            "the call `notes_get` got no response",
            Some("connection refused".into()),
        );

        assert_eq!(error.status(), None);
        assert_eq!(error.kind(), ErrorKind::Connect);
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

        let failed: Error = Error::<Shape>::failed(ErrorKind::Timeout, "got no response").into();

        assert_eq!(failed.status(), None);
        assert_eq!(failed.kind(), ErrorKind::Timeout);
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
