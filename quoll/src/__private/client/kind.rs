//! The kind of a failure of reqwest or of a middleware chain, which the client's errors, `retry` and `trace` read.
//!
//! Two fixed rules, with no search down an error's sources but through reqwest's own: a middleware's error is
//! `Middleware`, unless it is a reqwest error passed through, or quoll's `HeadTimedOut`; and a reqwest error is of the
//! kind its flags and its hyper error name, `Closed` for any other failure once the request was sent.

use std::error::Error as StdError;

use crate::ErrorKind;
use crate::client::HeadTimedOut;

/// The kind of `error`, a failure of the chain: a reqwest error, or a chain's.
///
/// A middleware's own error is [`ErrorKind::Middleware`], whatever it holds: a token source that failed, a
/// `quoll::Error` of a call of the middleware's own, an error of the application's that wraps the chain's. Its cause
/// stays among its sources. Two are not the middleware's: a reqwest error it passes through as it is, of that error's
/// kind, and the `HeadTimedOut` of quoll's deadline on a stream, [`ErrorKind::Timeout`].
pub(crate) fn kind_of(error: &(dyn StdError + 'static)) -> ErrorKind {
    if let Some(error) = error.downcast_ref::<reqwest::Error>() {
        return of_reqwest(error);
    }

    match error.downcast_ref::<reqwest_middleware::Error>() {
        Some(reqwest_middleware::Error::Reqwest(error)) => of_reqwest(error),
        Some(reqwest_middleware::Error::Middleware(error)) => of_middleware(&**error),
        None => ErrorKind::Middleware,
    }
}

/// The kind of a middleware's own error, `error` itself and not its sources.
fn of_middleware(error: &(dyn StdError + 'static)) -> ErrorKind {
    if let Some(error) = error.downcast_ref::<reqwest::Error>() {
        return of_reqwest(error);
    }

    if error.is::<HeadTimedOut>() {
        return ErrorKind::Timeout;
    }

    ErrorKind::Middleware
}

/// The kind of a reqwest error, by its flags: a connection that could not be opened, in time or not, is `Connect`, as
/// nothing was sent; a deadline passed, `Timeout`; a body that cannot be read, `Body`; a request that cannot be built,
/// `Invalid`; a redirect, `Middleware`. Any other is a failure once the request was sent, of the kind its hyper error
/// names: [`of_hyper`].
fn of_reqwest(error: &reqwest::Error) -> ErrorKind {
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

    let hyper = sources(error).find_map(|source| source.downcast_ref::<hyper::Error>());

    hyper.map_or(ErrorKind::Closed, of_hyper)
}

/// What hyper says of a request it sent, which reqwest wraps:
///
/// - `Decode` for bytes of the peer that are not HTTP: a head it cannot parse, as a TLS port called over `http`
///   answers, or a message it did not wait for;
/// - `Invalid` for the caller's own failure, a body stream of its that failed, or a dispatch it dropped;
/// - `Closed` for a connection closed or cancelled, or one whose answer ended before it was whole, and for any other,
///   the I/O errors under it among them: a reset, an aborted connection, a broken pipe, an unexpected end.
///
/// HTTP/2 is not built, so that no error of h2 reaches it.
fn of_hyper(error: &hyper::Error) -> ErrorKind {
    if error.is_parse() || unexpected(error) {
        return ErrorKind::Decode;
    }

    if error.is_user() {
        return ErrorKind::Invalid;
    }

    ErrorKind::Closed
}

/// Whether `error` is hyper's message of the peer that it did not wait for, which no method of hyper's tells: its
/// `Display`, which has no cause to follow it.
fn unexpected(error: &hyper::Error) -> bool {
    error.to_string() == "received unexpected message from connection"
}

/// The sources of `error`, from the first.
fn sources<'a>(
    error: &'a (dyn StdError + 'static),
) -> impl Iterator<Item = &'a (dyn StdError + 'static)> {
    std::iter::successors(error.source(), |&error| error.source())
}
