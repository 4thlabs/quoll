//! The kind of a failure of reqwest or of a middleware chain, which the client's errors, `retry` and `trace` read.

use std::error::Error as StdError;

use crate::ErrorKind;
use crate::client::HeadTimedOut;

/// The kind of `error`, a failure of the chain or of a body's read: a reqwest error, a chain's, or a middleware's own,
/// whose sources are searched for the reqwest error it wraps, as reqwest-retry's `RetryError` keeps it.
pub(crate) fn kind_of(error: &(dyn StdError + 'static)) -> ErrorKind {
    let mut current = Some(error);

    while let Some(error) = current {
        if let Some(error) = error.downcast_ref::<reqwest::Error>() {
            return of_reqwest(error);
        }

        match error.downcast_ref::<reqwest_middleware::Error>() {
            Some(reqwest_middleware::Error::Reqwest(error)) => return of_reqwest(error),
            Some(reqwest_middleware::Error::Middleware(error)) => return kind_of(&**error),
            None => {}
        }

        if error.is::<HeadTimedOut>() {
            return ErrorKind::Timeout;
        }

        current = error.source();
    }

    ErrorKind::Middleware
}

/// The kind of a reqwest error. A connection that could not be opened in time is `Connect`, as nothing was sent; a
/// body's read past a deadline is `Timeout`.
pub(crate) fn of_reqwest(error: &reqwest::Error) -> ErrorKind {
    if error.is_connect() {
        return ErrorKind::Connect;
    }

    if error.is_timeout() {
        return ErrorKind::Timeout;
    }

    // reqwest's decoding is of a compression, which leaves a stream with no more bytes: the body is cut.
    if error.is_body() || error.is_decode() {
        return ErrorKind::Body;
    }

    if error.is_builder() {
        return ErrorKind::Invalid;
    }

    if error.is_redirect() {
        return ErrorKind::Middleware;
    }

    ErrorKind::Closed
}
