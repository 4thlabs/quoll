//! A call tried again after a failure that may pass, or as a closure of the application's decides, on the backoff of
//! backon.

use std::fmt;
use std::sync::Arc;
use std::time::{Duration, SystemTime};

use backon::{BackoffBuilder, ExponentialBuilder};
use http::header::{DATE, RETRY_AFTER};
use http::{Extensions, HeaderMap, Method, StatusCode};
use reqwest::{Request, Response};
use reqwest_middleware::{Middleware, Next};

use super::request::HeadTimedOut;
use super::{Handled, own_route};
use crate::__private::client::{redacted, strip_urls};
use crate::rest::Route;

/// The wait before the first attempt again is drawn between it and twice it; it doubles after each, up to
/// [`LONGEST`].
const SHORTEST: Duration = Duration::from_millis(50);

/// The most the lower end of a wait grows to: a wait is drawn between it and twice it, 10 s at most.
const LONGEST: Duration = Duration::from_secs(5);

/// The longest wait [`Attempt::retry_after`] takes from a server, unless [`Retrying::max_wait`] sets another.
const MAX_WAIT: Duration = Duration::from_secs(60);

/// What the closure of [`Retrying::when`] is kept as.
type Decide = dyn Fn(&Attempt<'_>) -> Retry + Send + Sync;

/// Tries a call again, up to `times` more, after a failure that may pass, the rule of [`Attempt::transient`].
///
/// A failure that may pass is no response, the connection refused, reset or cut, a deadline of
/// [`timeout`](super::timeout) passed before the answer began, or an answer 502, 503 or 504, to a `GET`, `HEAD`,
/// `PUT` or `DELETE`: those may be sent twice with the effect of once, and any other is sent once, as a `POST` may
/// have been done before its connection was cut. The wait before each attempt is drawn at random between half a bound
/// and the bound, which doubles after each, from 100 ms to 10 s at most, so that clients do not all come back at
/// once.
///
/// [`when`](Retrying::when) decides otherwise, with a closure given each [`Attempt`]: a `429` waited out as its
/// `Retry-After` says, a `POST` that carries an `Idempotency-Key`. Whatever it decides, a request whose body cannot
/// be sent twice, a stream of the application's own, is sent once, and a call is tried `times` more at most. A route
/// whose result is a stream is tried again until its answer comes: what the stream gives after its 200 is the
/// caller's.
///
/// ```no_run
/// # use quoll::{client, rest};
/// let http = client::builder()
///     .with(client::retry(3))
///     .build_http()?;
/// # Ok::<(), quoll::rest::Error>(())
/// ```
pub fn retry(times: u32) -> Retrying {
    Retrying {
        times,
        max_wait: MAX_WAIT,
        decide: Arc::new(|attempt: &Attempt<'_>| attempt.transient()),
    }
}

/// The middleware of [`retry`]: a call tried again, as [`when`](Self::when) decides.
#[derive(Clone)]
pub struct Retrying {
    times: u32,
    max_wait: Duration,
    decide: Arc<Decide>,
}

impl Retrying {
    /// Decides after each attempt whether to try the call again, and when, with `decide`, in the place of
    /// [`Attempt::transient`], which it may still call for the cases it leaves alone.
    ///
    /// `decide` is given the [`Attempt`]: the call's [`Route`], its request, then the answer's status and headers or
    /// the error, and the attempt's number. It returns [`Retry::No`] to keep that answer, [`Retry::Backoff`] to try
    /// again after the wait of [`retry`], or [`Retry::After`] to try again after a wait of its own, as
    /// [`Attempt::retry_after`] reads it from the answer. An attempt past the `times` of `retry` is never made: the
    /// last answer is the call's.
    ///
    /// It is not called for a request whose body cannot be sent twice, a stream of the application's own: that
    /// request is sent once, and its answer is the call's.
    ///
    /// ```no_run
    /// use http::{Method, StatusCode};
    /// use quoll::client::{self, Attempt, Retry};
    ///
    /// # fn run() -> quoll::rest::Result<()> {
    /// let retry = client::retry(3).when(|attempt: &Attempt| {
    ///     let request = attempt.request();
    ///     let keyed = request.method() == Method::POST
    ///         && request.headers().contains_key("Idempotency-Key");
    ///
    ///     match attempt.status() {
    ///         Some(StatusCode::TOO_MANY_REQUESTS) => attempt.retry_after(),
    ///         Some(status) if keyed && status.is_server_error() => Retry::Backoff,
    ///         _ => attempt.transient(),
    ///     }
    /// });
    /// let http = client::builder().with(retry).build_http()?;
    /// # Ok(())
    /// # }
    /// ```
    #[must_use]
    pub fn when<F>(mut self, decide: F) -> Self
    where
        F: Fn(&Attempt<'_>) -> Retry + Send + Sync + 'static,
    {
        self.decide = Arc::new(decide);
        self
    }

    /// Sets the longest wait [`Attempt::retry_after`] takes from a server, one minute unless set: a longer
    /// `Retry-After` is waited for that long only.
    ///
    /// ```
    /// use std::time::Duration;
    ///
    /// use http::StatusCode;
    /// use quoll::client::{self, Attempt};
    ///
    /// let retry = client::retry(3)
    ///     .max_wait(Duration::from_secs(5))
    ///     .when(|attempt: &Attempt| match attempt.status() {
    ///         Some(StatusCode::TOO_MANY_REQUESTS) => attempt.retry_after(),
    ///         _ => attempt.transient(),
    ///     });
    /// ```
    #[must_use]
    pub fn max_wait(mut self, max_wait: Duration) -> Self {
        self.max_wait = max_wait;
        self
    }

    /// Sends `request`, then again as long as the decision says so and `times` allows it, with the waits of the
    /// backoff in between.
    async fn run(
        &self,
        request: Request,
        extensions: &mut Extensions,
        next: Next<'_>,
    ) -> reqwest_middleware::Result<Response> {
        let Some(mut sent) = request.try_clone() else {
            return next.run(request, extensions).await;
        };

        let mut backoff = ExponentialBuilder::new()
            .with_min_delay(SHORTEST)
            .with_max_delay(LONGEST)
            .with_factor(2.0)
            .with_jitter()
            .with_max_times(usize::try_from(self.times).unwrap_or(usize::MAX))
            .build();
        let mut number = 1;
        let contract = extensions.get::<Route>().is_some();

        loop {
            let mut result = next.clone().run(sent, extensions).await;

            // The error the closure is given shows no key of a query scheme added after `retry`, as the call's own; a
            // request that did not come from a contract keeps its URL whole, as reqwest gives it.
            if contract && let Err(error) = &mut result {
                strip_urls(error);
            }

            let decision = (self.decide)(&Attempt {
                route: own_route(&request, extensions),
                request: &request,
                result: &result,
                number,
                max_wait: self.max_wait,
            });

            // A wait of the closure's own takes one of the `times` too.
            let wait = match decision {
                Retry::No => None,
                Retry::Backoff => backoff.next(),
                Retry::After(wait) => backoff.next().map(|_| wait),
            };
            let Some(wait) = wait else {
                return result;
            };

            drop(result);
            tokio::time::sleep(wait).await;
            sent = request
                .try_clone()
                .expect("a request cloned once, its body bytes, clones again");
            number = number.saturating_add(1);
        }
    }
}

impl fmt::Debug for Retrying {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Retrying")
            .field("times", &self.times)
            .field("max_wait", &self.max_wait)
            .finish_non_exhaustive()
    }
}

/// The signature reqwest-middleware's `Middleware` has, as the `async-trait` it is written with spells it out.
impl Middleware for Retrying {
    fn handle<'life0, 'life1, 'life2, 'async_trait>(
        &'life0 self,
        request: Request,
        extensions: &'life1 mut Extensions,
        next: Next<'life2>,
    ) -> Handled<'async_trait>
    where
        'life0: 'async_trait,
        'life1: 'async_trait,
        'life2: 'async_trait,
        Self: 'async_trait,
    {
        Box::pin(self.run(request, extensions, next))
    }
}

/// What the closure of [`Retrying::when`] decides after an attempt.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Retry {
    /// The answer is the call's: no attempt again.
    No,
    /// Try again after the wait of [`retry`], which doubles after each attempt.
    Backoff,
    /// Try again after this wait.
    After(Duration),
}

/// An attempt of a call that [`Retrying::when`] decides on: the request, and what came back.
///
/// Its debug form shows the URL by its origin and its path, as the client's errors do.
#[derive(Clone, Copy)]
pub struct Attempt<'a> {
    route: Option<&'a Route>,
    request: &'a Request,
    result: &'a reqwest_middleware::Result<Response>,
    number: u32,
    max_wait: Duration,
}

impl fmt::Debug for Attempt<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Attempt")
            .field("route", &self.route)
            .field("method", self.request.method())
            .field("url", &redacted(self.request.url()))
            .field("number", &self.number)
            .field("status", &self.status())
            .field("error", &self.error())
            .finish_non_exhaustive()
    }
}

impl Attempt<'_> {
    /// The [`Route`] of a request of a contract to its api's own origin, as [`on_request`](super::on_request) is
    /// given it, or `None` for a request that did not come from the client of a contract, or sent to another origin.
    pub fn route(&self) -> Option<&Route> {
        self.route
    }

    /// The request as `retry` is given it: its method, its URL, and the headers of the route and of the middleware
    /// before `retry`, not those the middleware after it add, a credential or what [`on_request`](super::on_request)
    /// puts in.
    pub fn request(&self) -> &Request {
        self.request
    }

    /// The number of this attempt, 1 for the first.
    pub fn number(&self) -> u32 {
        self.number
    }

    /// The status of the answer, or `None` when the attempt got none.
    pub fn status(&self) -> Option<StatusCode> {
        self.result.as_ref().ok().map(Response::status)
    }

    /// The headers of the answer, or `None` when the attempt got none.
    pub fn headers(&self) -> Option<&HeaderMap> {
        self.result.as_ref().ok().map(Response::headers)
    }

    /// The error of an attempt that got no answer: no connection, a connection cut, a deadline passed, a middleware
    /// after `retry` that failed it.
    pub fn error(&self) -> Option<&reqwest_middleware::Error> {
        self.result.as_ref().err()
    }

    /// The rule of [`retry`] alone: [`Retry::Backoff`] after a failure that may pass, to a request that may be sent
    /// twice with the effect of once, and [`Retry::No`] after any other.
    ///
    /// A failure that may pass is no response, the connection refused, reset or cut, a deadline of
    /// [`timeout`](super::timeout) passed before the answer began, or an answer 502, 503 or 504. A request that may be
    /// sent twice is a `GET`, a `HEAD`, a `PUT` or a `DELETE`.
    pub fn transient(&self) -> Retry {
        let idempotent = [Method::GET, Method::HEAD, Method::PUT, Method::DELETE]
            .contains(self.request.method());
        let passing = match self.result {
            Ok(response) => matches!(
                response.status(),
                StatusCode::BAD_GATEWAY
                    | StatusCode::SERVICE_UNAVAILABLE
                    | StatusCode::GATEWAY_TIMEOUT
            ),
            Err(error) => may_pass(error),
        };

        if idempotent && passing {
            Retry::Backoff
        } else {
            Retry::No
        }
    }

    /// Try again when the answer's `Retry-After` says, in seconds or at an HTTP date, waiting no longer than
    /// [`max_wait`](Retrying::max_wait): [`Retry::After`] that wait, or [`Retry::Backoff`] for an answer with no
    /// `Retry-After` that can be read, or an attempt with no answer.
    ///
    /// A date is measured from the answer's `Date`, the server's clock, or from the client's when it has none, so that
    /// a client whose clock is ahead waits as long as the server asks; a date that is not after it is the backoff.
    ///
    /// It says when to try again, not whether the request may be sent twice: a `429` or a `503` with a `Retry-After`
    /// says the server did not act on it, which the closure of [`when`](Retrying::when) tells by the status.
    pub fn retry_after(&self) -> Retry {
        let Some(value) = self
            .headers()
            .and_then(|headers| headers.get(RETRY_AFTER))
            .and_then(|value| value.to_str().ok())
            .map(str::trim)
        else {
            return Retry::Backoff;
        };

        let seconds = !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit());

        // A number of seconds too large for a `u64` is a wait longer than any `max_wait`.
        let wait = if seconds {
            value.parse().map_or(self.max_wait, Duration::from_secs)
        } else if let Ok(at) = httpdate::parse_http_date(value) {
            match at.duration_since(self.answered_at()) {
                Ok(wait) if !wait.is_zero() => wait,
                _ => return Retry::Backoff,
            }
        } else {
            return Retry::Backoff;
        };

        Retry::After(wait.min(self.max_wait))
    }

    /// When the server answered, by its clock: the answer's `Date`, or the client's clock when it has none that can be
    /// read.
    fn answered_at(&self) -> SystemTime {
        self.headers()
            .and_then(|headers| headers.get(DATE))
            .and_then(|value| value.to_str().ok())
            .and_then(|value| httpdate::parse_http_date(value.trim()).ok())
            .unwrap_or_else(SystemTime::now)
    }
}

/// Whether an attempt that got no answer may pass if tried again: no connection, a deadline passed, a connection
/// reset, aborted or closed before its answer was whole, and the head of a stream's answer past the deadline of
/// `timeout`. A middleware's own failure, a body that cannot be read, a redirect, will fail again.
fn may_pass(error: &reqwest_middleware::Error) -> bool {
    let error = match error {
        reqwest_middleware::Error::Middleware(error) => {
            return error.downcast_ref::<HeadTimedOut>().is_some();
        }
        reqwest_middleware::Error::Reqwest(error) => error,
    };

    if error.is_timeout() || error.is_connect() {
        return true;
    }

    if error.is_body() || error.is_decode() || error.is_builder() || error.is_redirect() {
        return false;
    }

    if !error.is_request() {
        return false;
    }

    let Some(hyper) = source::<hyper::Error>(error) else {
        return false;
    };

    if hyper.is_incomplete_message() || hyper.is_canceled() {
        return true;
    }

    source::<std::io::Error>(hyper).is_some_and(|io| {
        matches!(
            io.kind(),
            std::io::ErrorKind::ConnectionReset | std::io::ErrorKind::ConnectionAborted
        )
    })
}

/// The first error of type `T` among the sources of `error`.
fn source<'a, T: std::error::Error + 'static>(
    error: &'a (dyn std::error::Error + 'static),
) -> Option<&'a T> {
    let mut current = error.source();

    while let Some(error) = current {
        if let Some(found) = error.downcast_ref::<T>() {
            return Some(found);
        }

        current = error.source();
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The decision of `retry_after` on an answer whose `Retry-After` is `value`, or that has none.
    fn after(value: Option<&str>, max_wait: Duration) -> Retry {
        dated(value, None, max_wait)
    }

    /// The decision of `retry_after` on an answer whose `Retry-After` is `value`, or that has none, and whose `Date`
    /// is `date`, or that has none.
    fn dated(value: Option<&str>, date: Option<&str>, max_wait: Duration) -> Retry {
        let mut answer = http::Response::builder().status(StatusCode::TOO_MANY_REQUESTS);

        if let Some(value) = value {
            answer = answer.header(RETRY_AFTER, value);
        }
        if let Some(date) = date {
            answer = answer.header(DATE, date);
        }

        let result = Ok(Response::from(answer.body("").expect("an answer")));
        let request = Request::new(
            Method::GET,
            reqwest::Url::parse("http://localhost/api").expect("a url"),
        );
        let attempt = Attempt {
            route: None,
            request: &request,
            result: &result,
            number: 1,
            max_wait,
        };

        attempt.retry_after()
    }

    #[test]
    fn retry_after_reads_seconds() {
        assert_eq!(
            after(Some("7"), MAX_WAIT),
            Retry::After(Duration::from_secs(7))
        );
        assert_eq!(after(Some(" 0 "), MAX_WAIT), Retry::After(Duration::ZERO));
    }

    #[test]
    fn retry_after_reads_an_http_date() {
        let at = httpdate::fmt_http_date(SystemTime::now() + Duration::from_secs(30));

        let Retry::After(wait) = after(Some(&at), MAX_WAIT) else {
            panic!("a wait");
        };

        assert!(
            wait > Duration::from_secs(28) && wait <= Duration::from_secs(30),
            "{wait:?}"
        );
    }

    #[test]
    fn retry_after_a_date_past_is_the_backoff() {
        assert_eq!(
            after(Some("Wed, 21 Oct 2015 07:28:00 GMT"), MAX_WAIT),
            Retry::Backoff
        );
    }

    #[test]
    fn retry_after_a_date_is_measured_from_the_answers_date() {
        // A server whose clock is a minute behind the client's asks for a wait of 20 s.
        let now = SystemTime::now() - Duration::from_secs(60);
        let date = httpdate::fmt_http_date(now);
        let at = httpdate::fmt_http_date(now + Duration::from_secs(20));

        assert_eq!(
            dated(Some(&at), Some(&date), MAX_WAIT),
            Retry::After(Duration::from_secs(20))
        );
        assert_eq!(
            dated(Some(&date), Some(&date), MAX_WAIT),
            Retry::Backoff,
            "a date that is the answer's own"
        );
    }

    #[test]
    fn retry_after_is_capped_by_max_wait() {
        assert_eq!(
            after(Some("3600"), Duration::from_secs(5)),
            Retry::After(Duration::from_secs(5))
        );
        assert_eq!(
            after(Some("99999999999999999999"), Duration::from_secs(5)),
            Retry::After(Duration::from_secs(5))
        );
    }

    #[test]
    fn retry_after_missing_or_unread_is_the_backoff() {
        for value in [
            None,
            Some(""),
            Some("soon"),
            Some("-1"),
            Some("+5"),
            Some("1.5"),
        ] {
            assert_eq!(after(value, MAX_WAIT), Retry::Backoff, "{value:?}");
        }
    }
}
