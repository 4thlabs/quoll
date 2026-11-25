//! A stream route followed: called again when its stream ends or fails, after a wait that doubles up to a bound, and
//! that starts again from the first once a connection has held.

use std::fmt;
use std::pin::Pin;
use std::task::{Context, Poll};
use std::time::Duration;

use futures_core::Stream;
use futures_util::{StreamExt, stream};
use http::StatusCode;
use tokio::time::Instant;

use crate::{Error, ErrorKind, Problem};

/// What a followed stream gives: its items, and what happens to its connection.
///
/// A match on it has an arm per state, with no `_`: a state added in a later version is a compile error where it must
/// be handled.
#[derive(Debug)]
pub enum Followed<T, E = Problem> {
    /// The route answered with its stream: from now on, no item goes by unseen. It comes first, and after each
    /// reconnection, when items may have been missed in between.
    Connected,
    /// An item of the stream.
    Item(T),
    /// An item that does not read as a `T`, an error of the kind [`Decode`](ErrorKind::Decode): the connection goes
    /// on.
    Skipped(Error<E>),
    /// The stream ended, or failed, or the call got no answer that it can follow: the route is called again after
    /// `wait`.
    Reconnecting {
        /// Why the connection is lost, `None` for a stream that ended.
        error: Option<Error<E>>,
        /// How long until the route is called again.
        wait: Duration,
    },
    /// The call failed in a way that calling again would not change: the last state, the stream ends after it.
    Stopped(Error<E>),
}

/// How a stream is followed: the wait before each call again, and when a connection counts as held.
///
/// The first call is made at once. The wait before each call after it is drawn at random between half a bound and the
/// bound, so that clients do not all come back at once after a restart of the server. The bound is
/// [`first`](Self::first) at the start, 1 s unless set, and doubles after each wait, up to
/// [`longest`](Self::longest), 30 s.
///
/// A connection holds once its stream has stayed open [`steady`](Self::steady), 10 s: the bound after it starts
/// again from `first`. Only the time counts, not the items: a server that sends an item then closes each connection
/// is called less and less often. The stream is read before the deadline is checked, so that a stream that ended
/// while nobody read it is not taken for one that held.
///
/// ```no_run
/// use std::time::Duration;
///
/// use quoll::client::Reconnect;
/// use quoll::JsonLines;
/// # use serde::Deserialize;
/// # use schemars::JsonSchema;
/// # #[derive(Debug, Deserialize, serde::Serialize, JsonSchema)]
/// # pub struct Line;
///
/// quoll::api! {
///     pub api Logs = "/api";
///
///     pub group Containers {
///         GET "/containers/{id}/logs" => logs(id: &str) -> JsonLines<Line>;
///     }
/// }
///
/// # fn run() -> quoll::Result<()> {
/// let containers = Logs::client("http://localhost:8080")?.containers();
/// let id = "web".to_owned();
///
/// let logs = Reconnect::new()
///     .first(Duration::from_millis(500))
///     .longest(Duration::from_secs(10))
///     .steady(Duration::from_secs(30))
///     .follow(async move || containers.logs(&id).await);
/// # let _ = logs;
/// # Ok(())
/// # }
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Reconnect {
    first: Duration,
    longest: Duration,
    steady: Duration,
    jitter: bool,
}

impl Default for Reconnect {
    fn default() -> Self {
        Self::new()
    }
}

impl Reconnect {
    /// A bound of 1 s at first, doubled after each wait up to 30 s, each wait drawn between half the bound and the
    /// bound, and back to 1 s once a connection has stayed open 10 s.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            first: Duration::from_secs(1),
            longest: Duration::from_secs(30),
            steady: Duration::from_secs(10),
            jitter: true,
        }
    }

    /// The bound of the first wait, and the one the waits start again from once a connection has held.
    #[must_use]
    pub const fn first(self, first: Duration) -> Self {
        Self { first, ..self }
    }

    /// The longest wait: the bound doubles up to it, and a [`first`](Self::first) beyond it is cut to it.
    #[must_use]
    pub const fn longest(self, longest: Duration) -> Self {
        Self { longest, ..self }
    }

    /// How long a connection stays open before it counts as held, and the waits start again from
    /// [`first`](Self::first).
    #[must_use]
    pub const fn steady(self, steady: Duration) -> Self {
        Self { steady, ..self }
    }

    /// Each wait its bound, not drawn: for a test that checks the times.
    #[must_use]
    pub const fn without_jitter(self) -> Self {
        Self {
            jitter: false,
            ..self
        }
    }

    /// Follows the stream that `open` calls the route for, as [`follow`] does, with these waits.
    pub fn follow<F, S, T, E>(self, open: F) -> Following<impl Stream<Item = Followed<T, E>>>
    where
        F: AsyncFnMut() -> Result<S, Error<E>>,
        S: Stream<Item = crate::Result<T>>,
    {
        let state = State {
            open,
            reconnect: self,
            stream: None,
            steady_at: None,
            held: false,
            bound: self.first.min(self.longest),
            wait: None,
            stopped: false,
        };

        Following {
            inner: Box::pin(stream::unfold(state, State::next)),
        }
    }
}

/// Follows the stream that `open` calls the route for: calls it at once, then again each time its stream ends or
/// fails, after a wait, as [`Reconnect::new`] sets it.
///
/// `open` is an async closure that makes the call and awaits it, `async move || api.group().route(..).await`: it
/// owns the group's client and the arguments, and lends them to each call. It works the same on a fake of the group,
/// which is called again too, so that a test scripts each connection. What is set per call goes in the closure, a
/// `.timeout(..)` for the head of each, a `.response()` to read its headers, or an argument read again at each call,
/// where to start from. The stream given is `Send` when the closure and its stream are, so that a task takes it, and
/// `'static` when the closure owns what it holds.
///
/// ```no_run
/// use futures_util::StreamExt;
/// use quoll::client::{self, Followed};
/// use quoll::Events;
/// # use serde::{Deserialize, Serialize};
/// # use schemars::JsonSchema;
/// # #[derive(Debug, Serialize, Deserialize, JsonSchema)]
/// # pub struct Change;
/// # fn reload_everything() {}
/// # fn apply(_: Change) {}
///
/// quoll::api! {
///     pub api Api = "/api";
///
///     pub group Feed {
///         GET "/changes?topic={topic}" => changes(topic: &str) -> Events<Change>;
///     }
/// }
///
/// # async fn run() -> quoll::Result<()> {
/// let feed = Api::client("http://localhost:8080")?.feed();
/// let topic = "notes".to_owned();
///
/// let mut changes = client::follow(async move || feed.changes(&topic).await);
///
/// while let Some(followed) = changes.next().await {
///     match followed {
///         // Items may have been missed before it: what they changed is not known.
///         Followed::Connected => reload_everything(),
///         Followed::Item(change) => apply(change),
///         Followed::Skipped(error) => {
///             eprintln!("a change that does not read: {error}");
///             reload_everything();
///         }
///         Followed::Reconnecting { error, wait } => eprintln!("again in {wait:?}: {error:?}"),
///         Followed::Stopped(error) => return Err(error),
///     }
/// }
/// # Ok(())
/// # }
/// ```
///
/// What each error gives, by its [kind](Error::kind):
///
/// | Where                 | Kind                                                               | What `follow` gives                                  |
/// | --------------------- | ------------------------------------------------------------------ | ---------------------------------------------------- |
/// | the call              | `Status(s)`, a 4xx but 408 and 429, or a 3xx                       | `Stopped`, and the stream ends                       |
/// | the call              | `Status(s)`, any other: a 5xx, a 408, a 429                        | `Reconnecting`                                       |
/// | the call              | `Invalid`: a URL it cannot build, a value it cannot write          | `Stopped`, and the stream ends                       |
/// | the call              | `Connect`, `Timeout`, `Closed`, `Body`, `Middleware`               | `Reconnecting`                                       |
/// | the call              | `Decode`: an answer that is not the route's, as a proxy's page     | `Reconnecting`                                       |
/// | the stream            | `Decode`: an item that does not read                               | `Skipped`, and the connection goes on                |
/// | the stream            | any other, an `event: problem` whatever its status                 | `Reconnecting`, the connection dropped               |
/// | the stream            | none: it ended                                                     | `Reconnecting` with no error                         |
///
/// A 401 that reaches `follow` stops it: [`bearer_from`](super::bearer_from) has already sent the call once more with
/// a new token. An `event: problem` of a 401 in the middle of the stream reconnects, and the token is asked again. An
/// error of the stream other than an item that does not read drops the connection, whether the stream would go on or
/// not. A [`retry`](super::retry) of the client still runs inside each call, before `follow` sees its error.
///
/// `follow` does not see a connection that stays open and silent, as a TCP connection whose other end is gone: TCP's
/// keep-alive does, `.http(|http| http.tcp_keepalive(..))` on the [`builder`](super::builder).
///
/// Two mistakes do not compile: a closure that returns the call without awaiting it, `move || feed.changes(&topic)`,
/// as a `Call` is not a future; and an async closure that does not await it, `async move || feed.changes(&topic)`,
/// whose result is a call and not a `Result`.
pub fn follow<F, S, T, E>(open: F) -> Following<impl Stream<Item = Followed<T, E>>>
where
    F: AsyncFnMut() -> Result<S, Error<E>>,
    S: Stream<Item = crate::Result<T>>,
{
    Reconnect::new().follow(open)
}

/// The stream [`follow`] gives, of [`Followed`] states.
///
/// It is `Unpin`, so that `.next()` reads it with no `pin!`, and `Send` when the closure and its stream are.
#[must_use = "a stream does nothing unless it is polled"]
pub struct Following<S> {
    inner: Pin<Box<S>>,
}

impl<S: Stream> Stream for Following<S> {
    type Item = S::Item;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<S::Item>> {
        self.inner.as_mut().poll_next(cx)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        self.inner.size_hint()
    }
}

impl<S> fmt::Debug for Following<S> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Following").finish_non_exhaustive()
    }
}

/// Where the following is, which each state given moves on.
struct State<F, S> {
    open: F,
    reconnect: Reconnect,
    /// The stream of the connection, while there is one.
    stream: Option<Pin<Box<S>>>,
    /// When the connection counts as held, until it does.
    steady_at: Option<Instant>,
    /// Whether the connection has held.
    held: bool,
    /// The bound of the next wait.
    bound: Duration,
    /// The wait before the next call, none before the first.
    wait: Option<Duration>,
    stopped: bool,
}

impl<F, S, T, E> State<F, S>
where
    F: AsyncFnMut() -> Result<S, Error<E>>,
    S: Stream<Item = crate::Result<T>>,
{
    async fn next(mut self) -> Option<(Followed<T, E>, Self)> {
        if self.stopped {
            return None;
        }

        loop {
            let Some(stream) = &mut self.stream else {
                let followed = self.connect().await;

                return Some((followed, self));
            };

            // The stream is polled before the deadline: a connection that ended while nobody read it is not taken
            // for one that held.
            let next = match self.steady_at {
                Some(steady_at) => {
                    let Ok(next) = tokio::time::timeout_at(steady_at, stream.next()).await else {
                        self.held = true;
                        self.steady_at = None;

                        continue;
                    };

                    next
                }
                None => stream.next().await,
            };

            let followed = match next {
                Some(Ok(item)) => Followed::Item(item),
                Some(Err(error)) if error.kind() == ErrorKind::Decode => {
                    Followed::Skipped(error.into_shape())
                }
                // Any other error drops the connection, whether its stream would go on or not.
                Some(Err(error)) => self.lost(Some(error.into_shape())),
                None => self.lost(None),
            };

            return Some((followed, self));
        }
    }

    /// Calls the route, after the wait unless it is the first call.
    async fn connect(&mut self) -> Followed<T, E> {
        if let Some(wait) = self.wait.take() {
            tokio::time::sleep(wait).await;
        }

        match (self.open)().await {
            Ok(stream) => {
                self.stream = Some(Box::pin(stream));
                self.steady_at = Some(Instant::now() + self.reconnect.steady);
                self.held = false;

                Followed::Connected
            }
            Err(error) if stops(error.kind()) => {
                self.stopped = true;

                Followed::Stopped(error)
            }
            Err(error) => Followed::Reconnecting {
                error: Some(error),
                wait: self.pause(),
            },
        }
    }

    /// Drops the connection: the waits start again from the first when it held.
    fn lost(&mut self, error: Option<Error<E>>) -> Followed<T, E> {
        self.stream = None;
        self.steady_at = None;

        if self.held {
            self.bound = self.reconnect.first.min(self.reconnect.longest);
        }

        Followed::Reconnecting {
            error,
            wait: self.pause(),
        }
    }

    /// The wait before the next call, drawn unless jitter is off, which doubles the next bound up to the longest.
    fn pause(&mut self) -> Duration {
        let bound = self.bound;
        let wait = if self.reconnect.jitter {
            bound.mul_f64(0.5 + fastrand::f64() / 2.0)
        } else {
            bound
        };

        self.bound = bound.saturating_mul(2).min(self.reconnect.longest);
        self.wait = Some(wait);

        wait
    }
}

/// Whether a call that failed with `kind` would fail the same if called again: an answer of a 4xx, but 408 and 429,
/// which say to try again later, or of a 3xx, a redirect not followed or a 304; or a call the client cannot send as
/// it is written.
fn stops(kind: ErrorKind) -> bool {
    match kind {
        ErrorKind::Status(status) => {
            let later = [StatusCode::REQUEST_TIMEOUT, StatusCode::TOO_MANY_REQUESTS];

            (status.is_client_error() && !later.contains(&status)) || status.is_redirection()
        }
        ErrorKind::Invalid => true,
        ErrorKind::Connect
        | ErrorKind::Timeout
        | ErrorKind::Closed
        | ErrorKind::Body
        | ErrorKind::Decode
        | ErrorKind::Middleware => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_4xx_but_408_and_429_a_3xx_and_a_misuse_stop() {
        let status = |code: u16| ErrorKind::Status(StatusCode::from_u16(code).expect("a status"));

        for code in [301, 304, 400, 401, 403, 404, 410, 422] {
            assert!(stops(status(code)), "{code} stops");
        }
        for code in [408, 429, 500, 502, 503, 504] {
            assert!(!stops(status(code)), "{code} calls again");
        }

        assert!(stops(ErrorKind::Invalid));
    }
}
