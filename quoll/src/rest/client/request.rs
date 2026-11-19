//! The helpers that change each request before it leaves: a closure of the application's, `User-Agent`,
//! `X-Request-Id` and a deadline.

use std::time::Duration;

use http::header::USER_AGENT;
use http::{HeaderName, HeaderValue};
use reqwest::Request;
use reqwest_middleware::Middleware;
use uuid::Uuid;

use super::{Failure, middleware, own_route};
use crate::rest::Route;

/// The header of [`request_id`].
const X_REQUEST_ID: HeaderName = HeaderName::from_static("x-request-id");

/// Calls `change` on each request of a contract to its api's own origin, with its [`Route`], before it leaves: what an
/// interceptor does elsewhere, as a header computed from the request.
///
/// A request with no `Route`, one that did not come from the client of a contract, and a hop of
/// [`redirects`](super::redirects) to another origin, go as they are: what `change` adds may be a credential, as the
/// signature below. It is added after `redirects`, so that it runs on each hop; added before, what it puts in goes
/// along to another origin, unless it is a header marked sensitive.
///
/// ```no_run
/// # use quoll::rest;
/// use reqwest::Request;
///
/// # fn sign(method: &http::Method, path: &str, body: &[u8]) -> http::HeaderValue { http::HeaderValue::from_static("") }
/// let sign = rest::client::on_request(|request: &mut Request, route: &rest::Route| {
///     let body = request.body().and_then(|body| body.as_bytes()).unwrap_or_default();
///     let signature = sign(request.method(), request.url().path(), body);
///
///     request.headers_mut().insert("X-Signature", signature);
/// });
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::redirects(5))
///     .with(sign)
///     .build();
/// ```
pub fn on_request<F>(change: F) -> impl Middleware
where
    F: Fn(&mut Request, &Route) + Send + Sync + 'static,
{
    middleware(move |mut request, extensions, next| {
        if let Some(route) = own_route(&request, extensions) {
            change(&mut request, route);
        }

        next.run(request, extensions)
    })
}

/// Sends `User-Agent: <name>` on each request that has none, a request of a contract or not, as reqwest's own
/// `user_agent` does.
///
/// A name that cannot be a header's value, as one with a line break, fails each call that would send it.
///
/// ```no_run
/// # use quoll::rest;
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::user_agent("notes-cli/0.1"))
///     .build();
/// ```
pub fn user_agent(name: impl Into<String>) -> impl Middleware {
    let value = HeaderValue::try_from(name.into()).ok();

    middleware(move |mut request, extensions, next| {
        if !request.headers().contains_key(USER_AGENT) {
            let Some(value) = value.clone() else {
                return Box::pin(async {
                    Err(Failure::fail(
                        "the user agent cannot be sent: it holds a control character or a line break",
                    ))
                });
            };

            request.headers_mut().insert(USER_AGENT, value);
        }

        next.run(request, extensions)
    })
}

/// Sends `X-Request-Id` with a new random UUID on each request that has none, a request of a contract or not, so that
/// the logs of the client and of the server tell the call by one id.
///
/// The middleware after it see the request with its id: added before [`retry`](super::retry), it gives every
/// attempt of a call the same id, and after it, each attempt its own. A route that sends an `X-Request-Id` of its own
/// keeps it.
///
/// ```no_run
/// # use quoll::rest;
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::request_id())
///     .with(rest::client::retry(3))
///     .build();
/// ```
pub fn request_id() -> impl Middleware {
    middleware(|mut request, extensions, next| {
        if !request.headers().contains_key(X_REQUEST_ID) {
            let mut buffer = Uuid::encode_buffer();
            let id = Uuid::new_v4().hyphenated().encode_lower(&mut buffer);
            let value = HeaderValue::from_str(id).expect("a UUID is a header's value");

            request.headers_mut().insert(X_REQUEST_ID, value);
        }

        next.run(request, extensions)
    })
}

/// Gives each attempt of a call, and each hop of its redirects, a deadline of `duration`, from when it is sent to the
/// end of its answer's body, as reqwest's own `timeout` does, whatever the place of `timeout` among the middleware:
/// it is a field of the request, which [`retry`](super::retry) and [`redirects`](super::redirects) copy. An attempt
/// that misses it fails with an error with no status.
///
/// `retry` tries again an attempt whose answer has not begun by then, but not one whose body is too slow: the body is
/// read once the middleware have returned the answer, past what they see.
///
/// A route whose result is a stream, `Events` or `JsonLines`, has a deadline for the head of its answer alone, its
/// status and headers: its body lasts as long as the server sends it, which a deadline would cut. That deadline
/// bounds the middleware after `timeout`, each attempt when it comes after `retry`, and is an error with no status
/// that `retry` tries again. A request that did not come from the client of a contract has a deadline, its body
/// included.
///
/// ```no_run
/// # use std::time::Duration;
/// # use quoll::rest;
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::retry(3))
///     .with(rest::client::timeout(Duration::from_secs(10)))
///     .build();
/// ```
pub fn timeout(duration: Duration) -> impl Middleware {
    middleware(move |mut request, extensions, next| {
        if !extensions.get::<Route>().is_some_and(|route| route.streams) {
            *request.timeout_mut() = Some(duration);

            return next.run(request, extensions);
        }

        Box::pin(async move {
            tokio::time::timeout(duration, next.run(request, extensions))
                .await
                .map_err(|_| reqwest_middleware::Error::middleware(HeadTimedOut))?
        })
    })
}

/// The head of a stream's answer that did not come by the deadline of [`timeout`], which [`retry`](super::retry)
/// tries again.
#[derive(Debug)]
pub(super) struct HeadTimedOut;

impl std::fmt::Display for HeadTimedOut {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("the answer did not begin by the deadline of the call")
    }
}

impl std::error::Error for HeadTimedOut {}
