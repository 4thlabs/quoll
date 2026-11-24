//! The kind of a failure of reqwest or of a middleware chain, which the client's errors, `retry` and `trace` read.
//!
//! It reads reqwest's own predicates, and nothing of the crates under reqwest: a middleware's error is `Middleware`,
//! unless it passes on a reqwest error or a chain's, whichever client it came from, or is quoll's `HeadTimedOut`; a
//! reqwest error is of the kind its predicates say, `Closed` for any other.

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

/// The status of the reqwest error that `error` is or passes on, by the rule of [`kind_of`]: an answer a middleware
/// turned into an error with `error_for_status`, which is that middleware's failure and not the call's status.
pub(crate) fn status_of(error: &(dyn StdError + 'static)) -> Option<http::StatusCode> {
    if let Some(error) = error.downcast_ref::<reqwest::Error>() {
        return error.status();
    }

    match error.downcast_ref::<reqwest_middleware::Error>()? {
        reqwest_middleware::Error::Reqwest(error) => error.status(),
        reqwest_middleware::Error::Middleware(error) => {
            let error: &(dyn StdError + 'static) = &**error;

            let passed_on = error.is::<reqwest::Error>() || error.is::<reqwest_middleware::Error>();

            passed_on.then(|| status_of(error)).flatten()
        }
    }
}

/// The kind of a reqwest error that dispatch or a middleware returns, by reqwest's predicates alone, in this order:
/// `is_builder`, `Invalid`; `is_connect`, `Connect`; `is_timeout`, `Timeout`; `is_body`, `Body`; `is_decode`, which
/// reqwest raises here for a middleware's own `json()` or read of a body, or a `Location` of a redirect that does not
/// parse, `is_redirect`, `is_upgrade` and a `status`, an answer a middleware turned into an error with
/// `error_for_status`, `Middleware`: a middleware's or reqwest's own doing, not the api's answer read by quoll; and any
/// other, `is_request` among them, `Closed`: the request left and no usable head came back.
///
/// A body quoll reads is not sorted here: where it reads, a failure is `Body`, or `Timeout` for a deadline.
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

    if error.is_decode() || error.is_redirect() || error.is_upgrade() || error.status().is_some() {
        return ErrorKind::Middleware;
    }

    ErrorKind::Closed
}
