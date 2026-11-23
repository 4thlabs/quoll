//! The answer of a route: its status, its headers and its body, what a handler returns and what a call's
//! `.response()` gives; and the status a handler may give it, a success or a redirection.

use std::error::Error as StdError;
use std::fmt;

use http::{HeaderMap, HeaderName, HeaderValue, StatusCode, Version};

use crate::rest::Result;

/// The answer of a route, as HTTP has it: its status, its version, its headers, and its body, a `T`.
///
/// A handler returns one, `Ok(Response::new(note))`, with the headers it sends: an `ETag`, a `Location`, a
/// `Cache-Control`. Its status is the one the contract says, 200 for a `T`, 201 for a `Created<T>`, 202 for an
/// `Accepted<T>`, 204 for a route with no result, unless the handler gives another with
/// [`with_status`](Response::with_status): a success or a redirection, a `PUT` that answers 201 when it creates and
/// 200 when it replaces, a 304 Not Modified, a 206. An error's status is an `Err(rest::Error)`, never a `Response`.
///
/// ```
/// use http::header::{ETAG, LOCATION};
/// use quoll::rest::Response;
///
/// let response = Response::new(3)
///     .header(ETAG, "\"7\"")
///     .header(LOCATION, format!("/api/notes/{}", 3));
///
/// assert_eq!(response.headers()[ETAG], "\"7\"");
/// assert_eq!(*response.body(), 3);
/// ```
///
/// On the client, a call's `.response()` gives the answer it got, whatever its status, its body read as the route's
/// value on a success and as the error of the answer otherwise: see [`Call`], with the feature `client`.
///
#[cfg_attr(feature = "client", doc = "[`Call`]: crate::rest::Call")]
#[cfg_attr(not(feature = "client"), doc = "[`Call`]: crate#features")]
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Response<T> {
    /// The status given: the answer's on the client, the handler's on the server, where `None` is the contract's.
    status: Option<StatusCode>,
    version: Version,
    headers: HeaderMap,
    body: T,
    /// The first header given to [`Response::header`] that is not one, which the server answers with a 500. A
    /// response the client reads never has one.
    invalid: Option<String>,
}

impl<T> Response<T> {
    /// A response with `body`, in HTTP/1.1, no headers, and the status the contract says: what a handler returns.
    pub fn new(body: T) -> Self {
        Self {
            status: None,
            version: Version::HTTP_11,
            headers: HeaderMap::new(),
            body,
            invalid: None,
        }
    }

    /// The answer the client read, or a fake's error gives: its status, its version, its headers and its body.
    pub(crate) fn from_parts(
        status: StatusCode,
        version: Version,
        headers: HeaderMap,
        body: T,
    ) -> Self {
        Self {
            status: Some(status),
            version,
            headers,
            body,
            invalid: None,
        }
    }

    /// The response with `status` in place of the one the contract says: a success or a redirection, which
    /// [`Status`] holds alone.
    ///
    /// ```
    /// use http::StatusCode;
    /// use quoll::rest::{Response, Status};
    ///
    /// let replaced = Response::new(3);
    /// let created = Response::new(3).with_status(Status::CREATED);
    ///
    /// assert_eq!(created.status(), StatusCode::CREATED);
    /// # let _ = replaced;
    /// ```
    ///
    /// A 204, a 205 and a 304 are answered without their body, which HTTP gives them none: a `Response<Note>` of a
    /// 304 holds a `Note` that is not sent. The OpenAPI document describes the contract's status alone.
    #[must_use]
    pub fn with_status(mut self, status: Status) -> Self {
        self.status = Some(status.0);
        self
    }

    /// Adds the header `name` with `value`, after those of that name it has already: a `Link` or a `Set-Cookie` may
    /// come more than once.
    ///
    /// `name` is a [`HeaderName`], as `http::header::ETAG`, or its text; `value` a [`HeaderValue`], a string or a
    /// number. A name or a value that is not one, a space in a name or a line break in a value, is kept out of the
    /// headers, and the server answers the response with a 500, logged without the value. A `Content-Type`, a
    /// `Content-Length` or a `Transfer-Encoding`, which quoll writes from the route's result, is kept, and answered
    /// with a 500 too. The `X-Content-Type-Options: nosniff` quoll sends with bytes stays whatever a handler gives.
    #[must_use]
    pub fn header<K, V>(mut self, name: K, value: V) -> Self
    where
        K: TryInto<HeaderName>,
        V: TryInto<HeaderValue>,
    {
        let Ok(name) = name.try_into() else {
            self.invalid
                .get_or_insert_with(|| "a header's name is not one".to_owned());
            return self;
        };
        let Ok(value) = value.try_into() else {
            self.invalid.get_or_insert_with(|| {
                format!("the value of the header `{name}` is not a header's value")
            });
            return self;
        };

        self.headers.append(name, value);
        self
    }

    /// The status: on the client, the one the answer came with; on the server, the one given with
    /// [`with_status`](Response::with_status), or 200 when none is, until quoll answers the one the contract says.
    pub fn status(&self) -> StatusCode {
        self.status.unwrap_or(StatusCode::OK)
    }

    /// The version of HTTP the answer came in, HTTP/1.1 for a response a handler builds.
    pub fn version(&self) -> Version {
        self.version
    }

    /// The headers.
    pub fn headers(&self) -> &HeaderMap {
        &self.headers
    }

    /// The headers, to change: what a middleware of the application's or a test does.
    pub fn headers_mut(&mut self) -> &mut HeaderMap {
        &mut self.headers
    }

    /// The body.
    pub fn body(&self) -> &T {
        &self.body
    }

    /// The body, leaving the status and the headers.
    pub fn into_body(self) -> T {
        self.body
    }

    /// The first header given to [`Response::header`] that was not one, which the server answers with a 500.
    #[cfg_attr(
        all(not(feature = "server"), not(test)),
        expect(dead_code, reason = "only the server answers a response")
    )]
    pub(crate) fn invalid(&self) -> Option<&str> {
        self.invalid.as_deref()
    }

    /// The status the handler gave, `None` for the contract's, the headers and the body, to answer.
    #[cfg_attr(
        all(not(feature = "server"), not(test)),
        expect(dead_code, reason = "only the server answers a response")
    )]
    pub(crate) fn into_answer(self) -> (Option<StatusCode>, HeaderMap, T) {
        (self.status, self.headers, self.body)
    }
}

impl<T, E> Response<Result<T, E>> {
    /// The route's value, or the error of the answer, to go up with `?`: what [`Call`]'s
    /// `.response()` gives once its status and its headers are read.
    ///
    /// # Errors
    ///
    /// The error of an answer with an error status, which holds its status, its detail and its body.
    ///
    #[cfg_attr(feature = "client", doc = "[`Call`]: crate::rest::Call")]
    #[cfg_attr(not(feature = "client"), doc = "[`Call`]: crate#features")]
    pub fn into_value(self) -> Result<T, E> {
        self.body
    }
}

impl<T: Default> Default for Response<T> {
    /// A response with the default of `T` and the status the contract says, as [`Response::new`] gives it:
    /// `Response::default()` for a route with no result, which answers 204.
    fn default() -> Self {
        Self::new(T::default())
    }
}

/// A status a handler gives its [`Response`]: a success, `2xx`, or a redirection, `3xx`. An error's status, `4xx` or
/// `5xx`, is an `Err(rest::Error)`, which answers its problem with it; an informational one, `1xx`, is no answer.
///
/// ```
/// use http::StatusCode;
/// use quoll::rest::Status;
///
/// assert_eq!(StatusCode::from(Status::NOT_MODIFIED), StatusCode::NOT_MODIFIED);
/// assert!(Status::try_from(StatusCode::IM_USED).is_ok());
/// assert!(Status::try_from(StatusCode::NOT_FOUND).is_err());
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Status(StatusCode);

impl Status {
    /// 200 OK.
    pub const OK: Self = Self(StatusCode::OK);
    /// 201 Created.
    pub const CREATED: Self = Self(StatusCode::CREATED);
    /// 202 Accepted.
    pub const ACCEPTED: Self = Self(StatusCode::ACCEPTED);
    /// 203 Non-Authoritative Information.
    pub const NON_AUTHORITATIVE_INFORMATION: Self = Self(StatusCode::NON_AUTHORITATIVE_INFORMATION);
    /// 204 No Content.
    pub const NO_CONTENT: Self = Self(StatusCode::NO_CONTENT);
    /// 205 Reset Content.
    pub const RESET_CONTENT: Self = Self(StatusCode::RESET_CONTENT);
    /// 206 Partial Content.
    pub const PARTIAL_CONTENT: Self = Self(StatusCode::PARTIAL_CONTENT);
    /// 300 Multiple Choices.
    pub const MULTIPLE_CHOICES: Self = Self(StatusCode::MULTIPLE_CHOICES);
    /// 301 Moved Permanently.
    pub const MOVED_PERMANENTLY: Self = Self(StatusCode::MOVED_PERMANENTLY);
    /// 302 Found.
    pub const FOUND: Self = Self(StatusCode::FOUND);
    /// 303 See Other.
    pub const SEE_OTHER: Self = Self(StatusCode::SEE_OTHER);
    /// 304 Not Modified.
    pub const NOT_MODIFIED: Self = Self(StatusCode::NOT_MODIFIED);
    /// 307 Temporary Redirect.
    pub const TEMPORARY_REDIRECT: Self = Self(StatusCode::TEMPORARY_REDIRECT);
    /// 308 Permanent Redirect.
    pub const PERMANENT_REDIRECT: Self = Self(StatusCode::PERMANENT_REDIRECT);

    /// The status, as http has it.
    pub fn as_status_code(self) -> StatusCode {
        self.0
    }
}

impl From<Status> for StatusCode {
    fn from(status: Status) -> Self {
        status.0
    }
}

impl TryFrom<StatusCode> for Status {
    type Error = InvalidStatus;

    /// `status`, when it is a success or a redirection.
    ///
    /// # Errors
    ///
    /// An [`InvalidStatus`] for an informational status, `1xx`, or an error's, `4xx` or `5xx`.
    fn try_from(status: StatusCode) -> std::result::Result<Self, InvalidStatus> {
        if status.is_success() || status.is_redirection() {
            return Ok(Self(status));
        }

        Err(InvalidStatus(status))
    }
}

/// A status that is not a [`Status`] a handler gives: an informational one, or an error's, which an
/// `Err(rest::Error)` answers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct InvalidStatus(StatusCode);

impl InvalidStatus {
    /// The status refused.
    pub fn status(&self) -> StatusCode {
        self.0
    }
}

impl fmt::Display for InvalidStatus {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let status = self.0.as_u16();

        if self.0.is_informational() {
            return write!(
                f,
                "{status} is an informational status, which answers nothing"
            );
        }

        write!(
            f,
            "{status} is an error's status, which a handler answers with an `Err(rest::Error)`"
        )
    }
}

impl StdError for InvalidStatus {}

#[cfg(test)]
mod tests {
    use http::header::{CONTENT_TYPE, LINK};

    use super::*;

    #[test]
    fn a_status_is_a_success_or_a_redirection() {
        for status in [
            StatusCode::OK,
            StatusCode::IM_USED,
            StatusCode::NOT_MODIFIED,
        ] {
            assert_eq!(Status::try_from(status).map(StatusCode::from), Ok(status));
        }

        for status in [
            StatusCode::CONTINUE,
            StatusCode::NOT_FOUND,
            StatusCode::INTERNAL_SERVER_ERROR,
        ] {
            let refused = Status::try_from(status).expect_err("not an answer");

            assert_eq!(refused.status(), status);
        }

        assert_eq!(
            Status::try_from(StatusCode::NOT_FOUND)
                .expect_err("an error's")
                .to_string(),
            "404 is an error's status, which a handler answers with an `Err(rest::Error)`"
        );
    }

    #[test]
    fn a_response_has_the_contracts_status_until_it_is_given_one() {
        let response = Response::new(());
        let given = Response::new(()).with_status(Status::NOT_MODIFIED);

        assert_eq!(response.into_answer().0, None);
        assert_eq!(given.status(), StatusCode::NOT_MODIFIED);
        assert_eq!(given.into_answer().0, Some(StatusCode::NOT_MODIFIED));
    }

    #[test]
    fn a_header_given_twice_is_sent_twice() {
        let response = Response::new(())
            .header(LINK, "</a>; rel=next")
            .header(LINK, "</b>; rel=last");

        assert_eq!(response.headers().get_all(LINK).iter().count(), 2);
        assert_eq!(response.invalid(), None);
    }

    #[test]
    fn a_header_that_is_not_one_is_kept_out_and_noted_without_its_value() {
        let named = Response::new(()).header("not a name", "1");
        let valued = Response::new(()).header("x-token", "secret\nline");

        assert!(named.headers().is_empty() && valued.headers().is_empty());
        assert_eq!(named.invalid(), Some("a header's name is not one"));
        assert_eq!(
            valued.invalid(),
            Some("the value of the header `x-token` is not a header's value")
        );
        assert_eq!(
            Response::new(())
                .header("bad name", "1")
                .header(CONTENT_TYPE, "x\n")
                .invalid(),
            Some("a header's name is not one"),
            "the first one is noted"
        );
    }
}
