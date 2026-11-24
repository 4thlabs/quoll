//! The kind of a failure of reqwest or of a middleware chain, which the client's errors, `retry` and `trace` read.

use std::error::Error as StdError;
use std::io;

use crate::client::HeadTimedOut;
use crate::{Error, ErrorKind};

/// The kind of `error`, a failure of the chain or of a body's read: a reqwest error, a chain's, or a middleware's own,
/// whose sources are searched for the reqwest error it wraps, as reqwest-retry's `RetryError` keeps it.
///
/// The search stops at a `quoll::Error`, which a middleware fails with when a call of its own failed, as a
/// `TokenSource` whose server does not answer: that failure is the middleware's, `Middleware`, and not the call's.
pub(crate) fn kind_of(error: &(dyn StdError + 'static)) -> ErrorKind {
    let mut current = Some(error);

    while let Some(error) = current {
        if error.is::<Error>() {
            return ErrorKind::Middleware;
        }

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
/// body's read past a deadline is `Timeout`. A request sent on a connection that was then closed, reset or cancelled
/// before the head of its answer is `Closed`; any other failure to read the answer, a head that is not HTTP's, as a
/// TLS port called over `http`, is `Decode`, which fails again.
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

    if closed(error) {
        return ErrorKind::Closed;
    }

    ErrorKind::Decode
}

/// Whether `error` comes from a connection closed, reset or cancelled: hyper's, or the I/O error under it.
fn closed(error: &reqwest::Error) -> bool {
    let mut current = error.source();

    while let Some(error) = current {
        if let Some(hyper) = error.downcast_ref::<hyper::Error>()
            && (hyper.is_incomplete_message() || hyper.is_canceled() || hyper.is_closed())
        {
            return true;
        }

        if let Some(io) = error.downcast_ref::<io::Error>() {
            return matches!(
                io.kind(),
                io::ErrorKind::ConnectionReset
                    | io::ErrorKind::ConnectionAborted
                    | io::ErrorKind::BrokenPipe
                    | io::ErrorKind::UnexpectedEof
            );
        }

        current = error.source();
    }

    false
}
