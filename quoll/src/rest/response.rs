//! The answer of a route: its status, its headers and its body, what a handler returns and what a call's
//! `.response()` gives; and the status a handler may give it, a success or a redirection.

use std::error::Error as StdError;
use std::fmt;

use http::{HeaderMap, HeaderName, HeaderValue, StatusCode, Version};

use crate::rest::{ByteStream, Bytes, Events, JsonLines, Result, Text};

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
    /// A 204 or a 205, [`Status::NO_CONTENT`] and [`Status::RESET_CONTENT`], a [`NoContent`], says there is no
    /// body: it is the status of a route whose result may be empty, which [`Allows`] lists, and the client reads
    /// it as `()`, `None`, an empty text or bytes, or a stream that ends at once. A route of a value, `-> Note`, a
    /// `Created<Note>` or an `Accepted<Note>`, takes none: it does not compile, and the route returns
    /// `-> Option<Note>`.
    ///
    /// ```
    /// use quoll::rest::{Response, Status};
    ///
    /// # struct Note;
    /// let missing: Response<Option<Note>> = Response::new(None).with_status(Status::NO_CONTENT);
    /// # let _ = missing;
    /// ```
    ///
    /// ```compile_fail
    /// use quoll::rest::{Response, Status};
    ///
    /// let note = Response::new(3).with_status(Status::NO_CONTENT);
    /// ```
    ///
    /// A route with no result that answers 201 when it creates and 204 when it replaces gives each in its branch:
    ///
    /// ```
    /// use quoll::rest::{Response, Status};
    ///
    /// fn put(created: bool) -> Response<()> {
    ///     let response = Response::new(());
    ///
    ///     if created {
    ///         return response.with_status(Status::CREATED);
    ///     }
    ///
    ///     response.with_status(Status::NO_CONTENT)
    /// }
    /// # assert_eq!(put(false).status(), http::StatusCode::NO_CONTENT);
    /// ```
    ///
    /// A 204, a 205 and a 304 are answered without their body, which HTTP gives them none: the value the `Response`
    /// holds is not sent, a `Some(note)` given a 204 as a `Note` given a 304. The OpenAPI document describes the
    /// contract's status, and a 204 on a route whose result may be empty.
    #[must_use]
    pub fn with_status<S>(mut self, status: S) -> Self
    where
        S: IntoStatus,
        S::Body: Allows<T>,
    {
        self.status = Some(status.into_status_code());
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
/// `5xx`, is an `Err(rest::Error)`, which answers its problem with it; an informational one, `1xx`, is no answer; a
/// 204 or a 205 is a [`NoContent`], of a route with no result or an `Option<T>`.
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
    /// 204 No Content, of a route whose result may be empty: a [`NoContent`].
    pub const NO_CONTENT: NoContent = NoContent(StatusCode::NO_CONTENT);
    /// 205 Reset Content, of a route whose result may be empty: a [`NoContent`].
    pub const RESET_CONTENT: NoContent = NoContent(StatusCode::RESET_CONTENT);
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

    /// `status`, when it is a success or a redirection, but a 204 or a 205, which a [`NoContent`] holds.
    ///
    /// # Errors
    ///
    /// An [`InvalidStatus`] for an informational status, `1xx`, an error's, `4xx` or `5xx`, a 204 or a 205.
    fn try_from(status: StatusCode) -> std::result::Result<Self, InvalidStatus> {
        if no_content(status) {
            return Err(InvalidStatus(status));
        }
        if status.is_success() || status.is_redirection() {
            return Ok(Self(status));
        }

        Err(InvalidStatus(status))
    }
}

/// A status of no content, a 204 or a 205, [`Status::NO_CONTENT`] and [`Status::RESET_CONTENT`]: the status of a
/// route whose result may be empty, which [`Allows`] lists, as [`Response::with_status`] says.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct NoContent(StatusCode);

impl NoContent {
    /// The status, as http has it.
    pub fn as_status_code(self) -> StatusCode {
        self.0
    }
}

impl From<NoContent> for StatusCode {
    fn from(status: NoContent) -> Self {
        status.0
    }
}

/// Whether `status` is one of no content, a 204 or a 205.
pub(crate) fn no_content(status: StatusCode) -> bool {
    matches!(status, StatusCode::NO_CONTENT | StatusCode::RESET_CONTENT)
}

impl TryFrom<StatusCode> for NoContent {
    type Error = InvalidStatus;

    /// `status`, when it is a 204 or a 205: one another API answered, given again.
    ///
    /// # Errors
    ///
    /// An [`InvalidStatus`] for any other status.
    fn try_from(status: StatusCode) -> std::result::Result<Self, InvalidStatus> {
        if no_content(status) {
            return Ok(Self(status));
        }

        Err(InvalidStatus(status))
    }
}

/// A status a [`Response`] is given with [`Response::with_status`]: a [`Status`], or a [`NoContent`], which only a
/// route whose result may be empty takes.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not a status a handler gives",
    label = "a `rest::Status` or a `rest::NoContent`",
    note = "a status is one of `rest::Status`'s, `Status::CREATED`, or `Status::try_from(status)` for an `http::StatusCode`; a 204 or a 205 is `Status::NO_CONTENT`, or `NoContent::try_from(status)`"
)]
pub trait IntoStatus: sealed::Sealed {
    /// What the status says of the body: any for a [`Status`], none for a [`NoContent`].
    #[doc(hidden)]
    type Body;

    /// The status, as http has it.
    #[doc(hidden)]
    fn into_status_code(self) -> StatusCode;
}

impl IntoStatus for Status {
    type Body = sealed::AnyBody;

    fn into_status_code(self) -> StatusCode {
        self.0
    }
}

impl IntoStatus for NoContent {
    type Body = sealed::NoBody;

    fn into_status_code(self) -> StatusCode {
        self.0
    }
}

/// What a status says of the body allows of a [`Response`]'s value `T`: any, for a
/// [`Status`]; for a 204 or a 205, a [`NoContent`], a result that may be empty alone, `()`, an `Option<T>`, a `Text`,
/// `Bytes`, `Events<T>`, `JsonLines<T>` or a `ByteStream`. A value, `T`, a `Created<T>` or an `Accepted<T>`, is not:
/// an `Option<T>` is.
#[diagnostic::on_unimplemented(
    message = "a 204 or a 205 says there is no body, and a `{T}` is a value that the route answers",
    label = "a status of no content, on a route of a `{T}`",
    note = "a route that may answer no content returns an `Option`: in the contract, `-> Option<{T}>` in place of `-> {T}`, `-> Created<{T}>` or `-> Accepted<{T}>`, and in the handler, `Response::new(None)` with this status"
)]
pub trait Allows<T>: sealed::Sealed {}

impl<T> Allows<T> for sealed::AnyBody {}

/// Implements `Allows` of a 204 on each result that may be empty, `T` its parameter where it has one.
macro_rules! may_be_empty {
    ($($ty:ty $(=> $param:ident)?),* $(,)?) => {
        $(
            impl$(<$param>)? Allows<$ty> for sealed::NoBody {}
        )*
    };
}

may_be_empty! {
    (),
    Option<T> => T,
    Text,
    Bytes,
    Events<T> => T,
    JsonLines<T> => T,
    ByteStream,
}

mod sealed {
    /// What only the statuses of quoll implement.
    pub trait Sealed {}

    impl Sealed for super::Status {}
    impl Sealed for super::NoContent {}
    impl Sealed for AnyBody {}
    impl Sealed for NoBody {}

    /// A status that leaves the body as it is.
    #[derive(Debug)]
    pub struct AnyBody;

    /// A status of no content, which a body may not have.
    #[derive(Debug)]
    pub struct NoBody;
}

/// A status that is not a [`Status`] a handler gives: an informational one, an error's, which an `Err(rest::Error)`
/// answers, or one of no content, which a [`NoContent`] holds; or, for `NoContent::try_from`, any status of a body.
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
        if !no_content(self.0) && (self.0.is_success() || self.0.is_redirection()) {
            return write!(
                f,
                "{status} is not a status of no content: it is a `Status`, `Status::try_from(status)`"
            );
        }
        if no_content(self.0) {
            return write!(
                f,
                "{status} says there is no body: it is a `NoContent`, `NoContent::try_from(status)`, of a route whose result may be empty"
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
            StatusCode::NO_CONTENT,
            StatusCode::RESET_CONTENT,
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
    fn a_status_of_no_content_is_one_of_its_own() {
        let none: Response<Option<u32>> = Response::new(None).with_status(Status::NO_CONTENT);
        let reset = Response::new(()).with_status(Status::RESET_CONTENT);

        assert_eq!(none.status(), StatusCode::NO_CONTENT);
        assert_eq!(reset.status(), StatusCode::RESET_CONTENT);
        assert_eq!(
            Status::try_from(StatusCode::NO_CONTENT)
                .expect_err("one of no content")
                .to_string(),
            "204 says there is no body: it is a `NoContent`, `NoContent::try_from(status)`, of a route whose result may be empty"
        );

        let text = Response::new(Text::from("")).with_status(Status::NO_CONTENT);
        let again = NoContent::try_from(StatusCode::RESET_CONTENT).expect("one of no content");

        assert_eq!(text.status(), StatusCode::NO_CONTENT);
        assert_eq!(StatusCode::from(again), StatusCode::RESET_CONTENT);
        assert_eq!(
            NoContent::try_from(StatusCode::OK)
                .expect_err("a status of a body")
                .to_string(),
            "200 is not a status of no content: it is a `Status`, `Status::try_from(status)`"
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
