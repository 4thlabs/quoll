//! The kind of a failure of reqwest or of a middleware chain, which the client's errors, `retry` and `trace` read.
//!
//! It reads reqwest's own predicates, and nothing of the crates under reqwest: a middleware's error is `Middleware`,
//! unless it passes on a reqwest error or a chain's, or is quoll's `HeadTimedOut`; a reqwest error is of the kind its
//! predicates say, `Closed` for any other.

use std::error::Error as StdError;

use crate::ErrorKind;
use crate::client::HeadTimedOut;

/// The kind of `error`, a failure of the chain: a reqwest error, or a chain's.
///
/// A middleware's own error is [`ErrorKind::Middleware`], whatever it holds: a token source that failed, a
/// `quoll::Error` of a call of the middleware's own, an error of the application's that wraps the chain's. Its cause
/// stays among its sources. Two are not the middleware's: a reqwest error, or an error of the chain, that it passes on
/// as it is, of that error's kind, and the `HeadTimedOut` of quoll's deadline on a stream, [`ErrorKind::Timeout`].
pub(crate) fn kind_of(error: &(dyn StdError + 'static)) -> ErrorKind {
    if let Some(error) = error.downcast_ref::<reqwest::Error>() {
        return of_reqwest(error);
    }

    if let Some(error) = error.downcast_ref::<reqwest_middleware::Error>() {
        return match error {
            reqwest_middleware::Error::Reqwest(error) => of_reqwest(error),
            reqwest_middleware::Error::Middleware(error) => kind_of_middleware(&**error),
        };
    }

    ErrorKind::Middleware
}

/// The kind of a middleware's own error, `error` itself and not its sources: a reqwest error or a chain's passed on,
/// of its kind, quoll's `HeadTimedOut`, a `Timeout`, and any other, a `Middleware`.
fn kind_of_middleware(error: &(dyn StdError + 'static)) -> ErrorKind {
    if error.is::<HeadTimedOut>() {
        return ErrorKind::Timeout;
    }

    if error.is::<reqwest::Error>() || error.is::<reqwest_middleware::Error>() {
        return kind_of(error);
    }

    ErrorKind::Middleware
}

/// The kind of a reqwest error, by reqwest's predicates alone, in this order: `is_builder`, `Invalid`; `is_connect`,
/// `Connect`; `is_timeout`, `Timeout`; `is_body`, `Body`; `is_decode`, `Decode`; `is_redirect`, reqwest's own policy
/// that gave up, `Middleware`; and any other, `is_request` among them, `Closed`: the request left and no usable head
/// came back.
fn of_reqwest(error: &reqwest::Error) -> ErrorKind {
    if error.is_builder() {
        return ErrorKind::Invalid;
    }

    if error.is_connect() {
        return ErrorKind::Connect;
    }

    if error.is_timeout() {
        return ErrorKind::Timeout;
    }

    if error.is_body() {
        return ErrorKind::Body;
    }

    if error.is_decode() {
        return ErrorKind::Decode;
    }

    if error.is_redirect() {
        return ErrorKind::Middleware;
    }

    ErrorKind::Closed
}
