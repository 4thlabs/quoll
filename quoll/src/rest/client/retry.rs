//! A call tried again after a failure that may pass, on reqwest-retry.

use std::sync::Arc;
use std::time::Duration;

use http::{Method, StatusCode};
use reqwest::{Request, Response};
use reqwest_middleware::Middleware;
use reqwest_retry::policies::ExponentialBackoff;
use reqwest_retry::{
    Jitter, RetryTransientMiddleware, Retryable, RetryableStrategy, default_on_request_failure,
};

use super::middleware;
use super::request::HeadTimedOut;

/// The bound of the wait before the first attempt again, which doubles after each, up to [`LONGEST`]. A wait is
/// never shorter than half of it.
const SHORTEST: Duration = Duration::from_millis(100);

/// The bound of the wait before an attempt, once it has doubled enough.
const LONGEST: Duration = Duration::from_secs(10);

/// Tries a call again, up to `times` more, after a failure that may pass: no response, the connection refused, reset
/// or cut, a deadline of [`timeout`](super::timeout) passed before the answer began, or an answer 502, 503 or 504. The
/// wait before each attempt is drawn at random between 50 ms and a bound that doubles after each, from 100 ms to 10 s
/// at most, so that clients do not all come back at once.
///
/// It tries again the methods that may be sent twice with the effect of once, `GET`, `HEAD`, `PUT` and `DELETE`, and
/// sends any other once: a `POST` may have been done before its connection was cut. A request whose body cannot be
/// sent twice, a stream of the application's own, is sent once too. A route whose result is a stream is tried again
/// until its answer comes: what the stream gives after its 200 is the caller's.
///
/// ```no_run
/// # use quoll::rest;
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::retry(3))
///     .build();
/// ```
pub fn retry(times: u32) -> impl Middleware {
    let policy = ExponentialBackoff::builder()
        .retry_bounds(SHORTEST, LONGEST)
        .jitter(Jitter::Bounded)
        .base(2)
        .build_with_max_retries(times);
    let retrying = Arc::new(RetryTransientMiddleware::new_with_policy_and_strategy(
        policy, Transient,
    ));

    middleware(move |request, extensions, next| {
        if !may_retry(&request) {
            return next.run(request, extensions);
        }

        let retrying = Arc::clone(&retrying);

        Box::pin(async move { retrying.handle(request, extensions, next).await })
    })
}

/// Whether a request may be sent again: its method is idempotent, and its body, when it has one, is bytes.
fn may_retry(request: &Request) -> bool {
    let idempotent =
        [Method::GET, Method::HEAD, Method::PUT, Method::DELETE].contains(request.method());

    idempotent && request.body().is_none_or(|body| body.as_bytes().is_some())
}

/// What is tried again: a failure with no response that may pass, as reqwest-retry tells it, the head of a stream's
/// answer past the deadline of `timeout`, and 502, 503 and 504.
struct Transient;

impl RetryableStrategy for Transient {
    fn handle(&self, result: &reqwest_middleware::Result<Response>) -> Option<Retryable> {
        match result {
            Ok(response) => matches!(
                response.status(),
                StatusCode::BAD_GATEWAY
                    | StatusCode::SERVICE_UNAVAILABLE
                    | StatusCode::GATEWAY_TIMEOUT
            )
            .then_some(Retryable::Transient),
            Err(reqwest_middleware::Error::Middleware(error))
                if error.downcast_ref::<HeadTimedOut>().is_some() =>
            {
                Some(Retryable::Transient)
            }
            Err(error) => default_on_request_failure(error),
        }
    }
}
