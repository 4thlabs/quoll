//! The helpers that change each request before it leaves: a closure of the application's, `User-Agent`,
//! `X-Request-Id` and a deadline.

use std::future::Future;
use std::pin::pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Duration;

use futures_util::future::{Either, select};
use http::header::USER_AGENT;
use http::{HeaderName, HeaderValue};
use reqwest::{Request, Response};
use reqwest_middleware::Middleware;
use tokio::time::Instant;
use uuid::Uuid;

use super::{Failure, Helper, Rank, helper, own_route};
use crate::Route;

/// The header of [`request_id`].
const X_REQUEST_ID: HeaderName = HeaderName::from_static("x-request-id");

/// Calls `change` on each request of a contract to its api's own origin, before it leaves.
///
/// It is given the request and its [`Route`]: what an interceptor does elsewhere, as a header computed from the
/// request.
///
/// A request with no `Route`, one that did not come from the client of a contract, and a hop of
/// [`redirects`](super::redirects) to another origin, go as they are: what `change` adds may be a credential, as the
/// signature below. It comes after `redirects`, where a [`Builder`](super::Builder) puts it, so that it runs on each
/// hop; added before, what it puts in goes along to another origin, unless it is a header marked sensitive.
///
/// ```no_run
/// # use quoll::client;
/// use reqwest::Request;
///
/// # fn sign(_: &http::Method, _: &str, _: &[u8]) -> http::HeaderValue { http::HeaderValue::from_static("") }
/// # fn run() -> quoll::Result<()> {
/// let sign = client::on_request(|request: &mut Request, route: &quoll::Route| {
///     let body = request.body().and_then(|body| body.as_bytes()).unwrap_or_default();
///     let signature = sign(request.method(), request.url().path(), body);
///
///     request.headers_mut().insert("X-Signature", signature);
/// });
/// // After `redirects`, whatever the order: each hop is signed on the api's own origin only.
/// let http = client::builder()
///     .with(client::redirects(5))
///     .with(sign)
///     .build_http()?;
/// # Ok(())
/// # }
/// ```
pub fn on_request<F>(change: F) -> impl Middleware
where
    F: Fn(&mut Request, &Route) + Send + Sync + 'static,
{
    helper(Rank::Credential, move |mut request, extensions, next| {
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
/// # use quoll::client;
/// let http = client::builder()
///     .with(client::user_agent("notes-cli/0.1"))
///     .build_http()?;
/// # Ok::<(), quoll::Error>(())
/// ```
pub fn user_agent(name: impl Into<String>) -> impl Middleware {
    let value = HeaderValue::try_from(name.into()).ok();

    helper(Rank::UserAgent, move |mut request, extensions, next| {
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
/// The middleware after it see the request with its id: before [`retry`](super::retry), where a
/// [`Builder`](super::Builder) puts it, it gives every attempt of a call the same id, and after it, each attempt its
/// own. A route that sends an `X-Request-Id` of its own keeps it.
///
/// ```no_run
/// # use quoll::client;
/// let http = client::builder()
///     .with(client::request_id())
///     .with(client::retry(3))
///     .build_http()?;
/// # Ok::<(), quoll::Error>(())
/// ```
pub fn request_id() -> impl Middleware {
    helper(Rank::RequestId, |mut request, extensions, next| {
        if !request.headers().contains_key(X_REQUEST_ID) {
            let mut buffer = Uuid::encode_buffer();
            let id = Uuid::new_v4().hyphenated().encode_lower(&mut buffer);
            let value = HeaderValue::from_str(id).expect("a UUID is a header's value");

            request.headers_mut().insert(X_REQUEST_ID, value);
        }

        next.run(request, extensions)
    })
}

/// Gives each attempt of a call, and each hop of its redirects, a deadline of `duration`.
///
/// The deadline runs from when the attempt is sent to the end of its answer's body, as reqwest's own `timeout` does,
/// whatever the place of `timeout` among the middleware: it is a field of the request, which [`retry`](super::retry)
/// and [`redirects`](super::redirects) copy. An attempt that misses it fails with an error with no status.
///
/// `retry` tries again an attempt whose answer has not begun by then, but not one whose body is too slow: the body is
/// read once the middleware have returned the answer, past what they see.
///
/// A route whose result is a stream, `Events`, `JsonLines` or `ByteStream`, has a deadline for the head of its answer
/// alone, its status and headers: its body lasts as long as the server sends it, which a deadline would cut. That
/// deadline bounds the middleware after `timeout`, each attempt when it comes after `retry`, and starts again on each
/// hop of `redirects`; it is an error with no status that `retry` tries again. A request that did not come from the
/// client of a contract has a deadline, its body included.
///
/// A call given a deadline of its own, with [`quoll::Call::timeout`](crate::Call::timeout), has it in place of
/// `duration`.
///
/// ```no_run
/// # use std::time::Duration;
/// # use quoll::client;
/// let http = client::builder()
///     .with(client::retry(3))
///     .with(client::timeout(Duration::from_secs(10)))
///     .build_http()?;
/// # Ok::<(), quoll::Error>(())
/// ```
pub fn timeout(duration: Duration) -> impl Middleware {
    deadline(Some(duration))
}

/// The deadline of the calls given one of their own alone, which [`Builder`](super::Builder) puts when it is given no
/// [`timeout`]: a call's deadline is so kept for each attempt and each hop.
pub(crate) fn calls_timeout() -> Helper {
    deadline(None)
}

/// [`timeout`] with `default`, the deadline of a call that gives none of its own; with none, such a call has none.
fn deadline(default: Option<Duration>) -> Helper {
    helper(Rank::Timeout, move |mut request, extensions, next| {
        let call = extensions.get::<CallTimeout>().map(CallTimeout::keep);
        let Some(duration) = call.or(default) else {
            return next.run(request, extensions);
        };

        if !extensions.get::<Route>().is_some_and(|route| route.streams) {
            *request.timeout_mut() = Some(duration);

            return next.run(request, extensions);
        }

        Box::pin(async move {
            let deadline = HeadDeadline::new(duration);

            extensions.insert(deadline.clone());

            let answer = next.run(request, extensions);
            let passed = Box::pin(deadline.passed());
            let answered = match select(answer, passed).await {
                Either::Left((answered, _)) => answered,
                Either::Right(_) => Err(reqwest_middleware::Error::middleware(HeadTimedOut)),
            };

            extensions.remove::<HeadDeadline>();
            answered
        })
    })
}

/// The mark a chain that [`Builder`](super::Builder) builds puts on each of its requests, with a request initialiser:
/// the chain holds a [`timeout`], which keeps the deadline a call gives itself for each attempt and each hop, so that
/// the call need not bound itself. It is known before the request is sent, whatever the middleware before `timeout`
/// wait for.
#[derive(Debug, Clone, Copy)]
pub(crate) struct KeepsDeadline;

/// The deadline a call gives itself with [`quoll::Call::timeout`](crate::Call::timeout), in its request's
/// extensions, which [`timeout`] takes in place of its own and marks kept. Its clones share the mark.
#[derive(Debug, Clone)]
pub(crate) struct CallTimeout {
    duration: Duration,
    kept: Arc<AtomicBool>,
}

impl CallTimeout {
    pub(crate) fn new(duration: Duration) -> Self {
        Self {
            duration,
            kept: Arc::new(AtomicBool::new(false)),
        }
    }

    /// The deadline, which [`timeout`] keeps from now on.
    fn keep(&self) -> Duration {
        self.kept.store(true, Ordering::Relaxed);
        self.duration
    }

    /// The answer `sent` gives, the head of a stream's, unless the deadline passes before it, in a chain where no
    /// [`timeout`] has kept it by then: a client whose middleware were not built by [`Builder`](super::Builder),
    /// bounded so for the whole call rather than for each attempt. In such a chain, a middleware before its `timeout`
    /// that waits longer than the deadline, a rate limiter, fails the call: a chain of `Builder`'s is marked
    /// [`KeepsDeadline`], and never comes here.
    pub(crate) async fn kept_or_bounding(
        &self,
        sent: impl Future<Output = reqwest_middleware::Result<Response>>,
    ) -> reqwest_middleware::Result<Response> {
        let sent = pin!(sent);
        let passed = pin!(tokio::time::sleep(self.duration));

        match select(sent, passed).await {
            Either::Left((answered, _)) => answered,
            Either::Right(((), sent)) if self.kept.load(Ordering::Relaxed) => sent.await,
            Either::Right(_) => Err(reqwest_middleware::Error::middleware(HeadTimedOut)),
        }
    }
}

/// The deadline of the head of a stream's answer, which [`timeout`] puts in the request's extensions, and which
/// [`redirects`](super::redirects) starts again on each hop. Its clones share it.
#[derive(Debug, Clone)]
pub(super) struct HeadDeadline {
    duration: Duration,
    at: Arc<Mutex<Instant>>,
}

impl HeadDeadline {
    fn new(duration: Duration) -> Self {
        Self {
            duration,
            at: Arc::new(Mutex::new(Instant::now() + duration)),
        }
    }

    /// Starts the deadline again, from now.
    pub(super) fn restart(&self) {
        *self.at() = Instant::now() + self.duration;
    }

    /// Ends once the deadline has passed, as it stands when it is reached: a restart pushes it back.
    async fn passed(&self) {
        loop {
            let at = *self.at();

            if Instant::now() >= at {
                return;
            }

            tokio::time::sleep_until(at).await;
        }
    }

    fn at(&self) -> std::sync::MutexGuard<'_, Instant> {
        // An `Instant` is whole even when a holder panicked: the deadline stays good.
        self.at.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

/// The head of a stream's answer that did not come by the deadline of [`timeout`], which [`retry`](super::retry)
/// tries again.
#[derive(Debug)]
pub(crate) struct HeadTimedOut;

impl std::fmt::Display for HeadTimedOut {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("the answer did not begin by the deadline of the call")
    }
}

impl std::error::Error for HeadTimedOut {}
