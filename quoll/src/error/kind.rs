//! What went wrong in a call, or what an error answers: the kind of the error.

use std::fmt;

use http::StatusCode;

/// What an [`Error`](crate::Error) is: an error status, or what went wrong in a call of the client that got none.
/// [`Error::kind`](crate::Error::kind) always gives it.
///
/// It is `#[non_exhaustive]`: a match on it has a `_` arm, as a version may tell a new case apart.
///
/// ```
/// use http::StatusCode;
/// use quoll::{Error, ErrorKind};
///
/// fn what_to_do(error: &Error) -> &'static str {
///     match error.kind() {
///         ErrorKind::Status(StatusCode::NOT_FOUND) => "no such note: its detail says why",
///         ErrorKind::Status(status) if status.is_server_error() => "the other api is in trouble",
///         ErrorKind::Connect => "nothing listens there: the other api is gone",
///         ErrorKind::Timeout => "the other api is slow: try again later",
///         ErrorKind::Decode => "the other api answered something else: skip it",
///         _ => "the call failed: give up",
///     }
/// }
///
/// assert_eq!(what_to_do(&Error::not_found("no such note")), "no such note: its detail says why");
/// assert_eq!(what_to_do(&Error::failed(ErrorKind::Timeout, "slow")), "the other api is slow: try again later");
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum ErrorKind {
    /// An error of this status: what another API answered, its [detail](crate::Error::detail) when its body is the
    /// api's error shape; or an error a handler builds, 500 for [`Error::internal`](crate::Error::internal). It is
    /// what [`Error::status`](crate::Error::status) gives. An `event: problem` in a stream is of the problem's status,
    /// and ends the stream.
    Status(StatusCode),
    /// No connection could be opened: refused, a name that does not resolve, a TLS handshake that failed, a proxy
    /// that refused. Nothing was sent, so that any call may be tried again.
    Connect,
    /// A deadline passed before the answer: the call's `timeout`, the one of [`client::timeout`], or reqwest's own. On
    /// a whole result it covers the body too; on a stream, the head of the answer alone. The other api may have served
    /// the request.
    ///
    #[cfg_attr(
        feature = "client",
        doc = "[`client::timeout`]: crate::client::timeout"
    )]
    #[cfg_attr(not(feature = "client"), doc = "[`client::timeout`]: crate#features")]
    Timeout,
    /// The connection was open and the request sent, then the connection was closed or reset before the head of its
    /// answer came: a server that restarts, a proxy that drops an idle connection.
    Closed,
    /// The answer began, and its body could not be read to its end: a connection cut or reset in the middle of it, a
    /// compression that does not decode, or an item of a stream longer than the 1 MiB the client reads. A stream ends
    /// after it.
    Body,
    /// The answer came whole, with a success status, and is not the route's result: a content type that is not the
    /// route's, a text that is not UTF-8, JSON that is not the result. In a stream, an item that does not read as the
    /// route's item, after which the stream goes on. The other api answered: the connection works.
    Decode,
    /// A middleware failed the call itself: a credential that cannot be sent, a token that could not be had, a
    /// redirect to a request sent already or past its limit, or a middleware of the application's.
    Middleware,
    /// The call could not be sent as it was written: a client given a URL that is not one, a path parameter that is
    /// `None`, a header with a line break. It is the mistake of the code that called it, and fails again.
    Invalid,
}

impl ErrorKind {
    /// Whether the other api answered: an error [status](Self::Status), or a body that could not be read,
    /// [`Body`](Self::Body), or that is not the route's result, [`Decode`](Self::Decode). The connection works.
    #[must_use]
    pub const fn answered(&self) -> bool {
        matches!(self, Self::Status(_) | Self::Body | Self::Decode)
    }

    /// The kind as OpenTelemetry's `error.type` writes it: the status's code, `404`, or the kind in lowercase,
    /// `connect`, `timeout`...
    #[must_use]
    pub fn as_str(&self) -> &str {
        match self {
            Self::Status(status) => status.as_str(),
            Self::Connect => "connect",
            Self::Timeout => "timeout",
            Self::Closed => "closed",
            Self::Body => "body",
            Self::Decode => "decode",
            Self::Middleware => "middleware",
            Self::Invalid => "invalid",
        }
    }
}

/// What [`as_str`](ErrorKind::as_str) gives: `404`, `timeout`.
impl fmt::Display for ErrorKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}
