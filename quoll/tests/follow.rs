//! The rules of `client::follow` on a fake of a group, under paused time: the waits, what holds a connection, and what
//! each kind of error gives. A fake is called again as the client is, so that each test scripts its connections.
//!
//! The rules of time are checked here, with no socket. Paused time skips to the next timer as soon as the runtime
//! waits on nothing else, a socket included, so that a test with a real server checks them only where no connection
//! is open: `follow_server.rs`.

#![cfg(feature = "client")]

use std::collections::VecDeque;
use std::sync::Mutex;
use std::time::Duration;

use futures_util::{Stream, StreamExt, future, stream};
use http::StatusCode;
use quoll::client::{self, Followed, Reconnect};
use quoll::{Call, Error, ErrorKind, JsonLines};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use tokio::time::Instant;

const STEADY: Duration = Duration::from_secs(10);

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Change {
    kind: String,
}

quoll::api! {
    api Api = "/api";

    group Feed {
        GET "/changes" => changes() -> JsonLines<Change>;
    }
}

/// What a connection of the fake does.
enum Connection {
    /// The call fails with this error.
    Fails(Error),
    /// The stream sends these, then ends.
    Sends(Vec<quoll::Result<Change>>),
    /// The stream sends nothing, and ends after this long.
    Opens(Duration),
    /// The stream sends these, then stays open for ever.
    SendsAndStays(Vec<quoll::Result<Change>>),
}

/// A server that answers each call as the test says, then never again.
struct Fake {
    connections: Mutex<VecDeque<Connection>>,
    calls: Mutex<usize>,
}

impl Fake {
    /// A fake leaked for the test, so that the stream that follows it is `'static`.
    fn leak(connections: impl IntoIterator<Item = Connection>) -> &'static Self {
        Box::leak(Box::new(Self {
            connections: Mutex::new(connections.into_iter().collect()),
            calls: Mutex::new(0),
        }))
    }

    /// How many times the route was called.
    fn calls(&self) -> usize {
        *self.calls.lock().expect("no test panics with the lock")
    }
}

impl Feed for &'static Fake {
    fn changes(&self) -> impl Call<JsonLines<Change>> {
        *self.calls.lock().expect("no test panics with the lock") += 1;

        let connection = self
            .connections
            .lock()
            .expect("no test panics with the lock")
            .pop_front();

        async move {
            match connection {
                Some(Connection::Fails(error)) => Err(error),
                Some(Connection::Sends(changes)) => Ok(JsonLines::try_new(stream::iter(changes))),
                Some(Connection::Opens(duration)) => {
                    let ends = stream::once(tokio::time::sleep(duration))
                        .filter_map(|()| future::ready(None::<quoll::Result<Change>>));

                    Ok(JsonLines::try_new(ends))
                }
                Some(Connection::SendsAndStays(changes)) => Ok(JsonLines::try_new(
                    stream::iter(changes).chain(stream::pending()),
                )),
                None => future::pending().await,
            }
        }
    }
}

fn change(kind: &str) -> Change {
    Change {
        kind: kind.to_owned(),
    }
}

fn failed(kind: ErrorKind) -> Connection {
    Connection::Fails(Error::failed(kind, "the call failed"))
}

fn answered(status: StatusCode) -> Connection {
    Connection::Fails(Error::new(status, "the server answered"))
}

/// What a followed state is, by name, to compare lists of them.
fn name(followed: &Followed<Change>) -> String {
    match followed {
        Followed::Connected => "connected".to_owned(),
        Followed::Item(change) => format!("item {}", change.kind),
        Followed::Skipped(error) => format!("skipped {}", error.kind()),
        Followed::Reconnecting { error: None, wait } => {
            format!("ended, again in {}s", wait.as_secs())
        }
        Followed::Reconnecting {
            error: Some(error),
            wait,
        } => format!("failed {}, again in {}s", error.kind(), wait.as_secs()),
        Followed::Stopped(error) => format!("stopped {}", error.kind()),
    }
}

/// The fake followed with no jitter.
fn followed(fake: &'static Fake) -> impl Stream<Item = Followed<Change>> + Send + Unpin + 'static {
    Reconnect::new()
        .without_jitter()
        .follow(async move || fake.changes().await)
}

/// The first `count` states that following `fake` gives, by name.
async fn first(fake: &'static Fake, count: usize) -> Vec<String> {
    followed(fake)
        .take(count)
        .map(|followed| name(&followed))
        .collect()
        .await
}

#[tokio::test(start_paused = true)]
async fn calls_at_once_then_after_a_wait_that_doubles_while_nothing_answers() {
    let fake = Fake::leak([
        failed(ErrorKind::Connect),
        failed(ErrorKind::Connect),
        failed(ErrorKind::Connect),
        Connection::Sends(Vec::new()),
    ]);
    let started = Instant::now();

    let followed = first(fake, 4).await;

    assert_eq!(
        followed,
        [
            "failed connect, again in 1s",
            "failed connect, again in 2s",
            "failed connect, again in 4s",
            "connected"
        ]
    );
    assert_eq!(started.elapsed(), Duration::from_secs(7));
}

#[tokio::test(start_paused = true)]
async fn keeps_doubling_while_each_connection_closes_at_once() {
    let fake = Fake::leak([
        Connection::Sends(Vec::new()),
        Connection::Sends(Vec::new()),
        Connection::Sends(Vec::new()),
    ]);
    let started = Instant::now();

    let followed = first(fake, 5).await;

    // A proxy that answers then closes: the answer does not reset the wait.
    assert_eq!(
        followed,
        [
            "connected",
            "ended, again in 1s",
            "connected",
            "ended, again in 2s",
            "connected"
        ]
    );
    assert_eq!(started.elapsed(), Duration::from_secs(3));
}

#[tokio::test(start_paused = true)]
async fn starts_again_from_the_first_wait_once_a_connection_held() {
    let fake = Fake::leak([
        Connection::Sends(Vec::new()),
        Connection::Opens(STEADY + Duration::from_secs(1)),
        Connection::Sends(Vec::new()),
    ]);

    let followed = first(fake, 6).await;

    assert_eq!(
        followed,
        [
            "connected",
            "ended, again in 1s",
            "connected",
            "ended, again in 1s",
            "connected",
            "ended, again in 2s"
        ]
    );
}

#[tokio::test(start_paused = true)]
async fn a_connection_that_ends_just_before_steady_has_not_held() {
    let fake = Fake::leak([
        Connection::Sends(Vec::new()),
        Connection::Opens(STEADY - Duration::from_millis(1)),
        Connection::Sends(Vec::new()),
    ]);

    let followed = first(fake, 4).await;

    assert_eq!(
        followed,
        [
            "connected",
            "ended, again in 1s",
            "connected",
            "ended, again in 2s"
        ]
    );
}

#[tokio::test(start_paused = true)]
async fn an_item_does_not_make_a_connection_held() {
    let fake = Fake::leak([
        Connection::Sends(Vec::new()),
        Connection::Sends(vec![Ok(change("note"))]),
        Connection::Sends(Vec::new()),
    ]);

    let followed = first(fake, 6).await;

    assert_eq!(
        followed,
        [
            "connected",
            "ended, again in 1s",
            "connected",
            "item note",
            "ended, again in 2s",
            "connected"
        ]
    );
}

#[tokio::test(start_paused = true)]
async fn does_not_take_a_slow_reader_for_a_connection_that_held() {
    let fake = Fake::leak([Connection::Sends(Vec::new()), Connection::Sends(Vec::new())]);
    let mut followed = followed(fake);

    assert!(matches!(followed.next().await, Some(Followed::Connected)));

    // The caller reads again long after the connection, which closed at once.
    tokio::time::sleep(2 * STEADY).await;

    let ended = followed.next().await.expect("the end of the connection");

    assert_eq!(name(&ended), "ended, again in 1s");
    assert!(matches!(followed.next().await, Some(Followed::Connected)));

    let ended = followed.next().await.expect("the end of the connection");

    // Doubled, rather than back to 1 s.
    assert_eq!(name(&ended), "ended, again in 2s");
}

#[tokio::test(start_paused = true)]
async fn never_waits_longer_than_the_longest() {
    let fake = Fake::leak((0..8).map(|_| failed(ErrorKind::Connect)));

    let followed = first(fake, 8).await;
    let waits: Vec<_> = followed
        .iter()
        .map(|name| name.trim_start_matches("failed connect, again in "))
        .collect();

    assert_eq!(waits, ["1s", "2s", "4s", "8s", "16s", "30s", "30s", "30s"]);
}

#[tokio::test(start_paused = true)]
async fn takes_the_waits_it_is_given() {
    let fake = Fake::leak([
        failed(ErrorKind::Connect),
        failed(ErrorKind::Connect),
        failed(ErrorKind::Connect),
        Connection::Opens(Duration::from_secs(3)),
        failed(ErrorKind::Connect),
    ]);
    let reconnect = Reconnect::new()
        .without_jitter()
        .first(Duration::from_millis(500))
        .longest(Duration::from_secs(1))
        .steady(Duration::from_secs(2));

    let waits: Vec<_> = reconnect
        .follow(async move || fake.changes().await)
        .take(6)
        .filter_map(|followed| async move {
            match followed {
                Followed::Reconnecting { wait, .. } => Some(wait),
                _ => None,
            }
        })
        .collect()
        .await;

    // Doubled up to the longest, then the first again once the connection of 3 s held.
    assert_eq!(
        waits,
        [500, 1000, 1000, 500, 1000].map(Duration::from_millis)
    );
}

#[tokio::test(start_paused = true)]
async fn a_first_wait_beyond_the_longest_is_cut_to_it() {
    let fake = Fake::leak([failed(ErrorKind::Connect), failed(ErrorKind::Connect)]);
    let reconnect = Reconnect::new()
        .without_jitter()
        .first(Duration::from_secs(60))
        .longest(Duration::from_secs(5));

    let followed: Vec<_> = reconnect
        .follow(async move || fake.changes().await)
        .take(2)
        .map(|followed| name(&followed))
        .collect()
        .await;

    assert_eq!(
        followed,
        ["failed connect, again in 5s", "failed connect, again in 5s"]
    );
}

#[tokio::test(start_paused = true)]
async fn draws_each_wait_between_half_its_bound_and_its_bound() {
    let fake = Fake::leak((0..6).map(|_| failed(ErrorKind::Connect)));
    let started = Instant::now();

    let waits: Vec<Duration> = client::follow(async move || fake.changes().await)
        .take(6)
        .filter_map(|followed| async move {
            match followed {
                Followed::Reconnecting { wait, .. } => Some(wait),
                _ => None,
            }
        })
        .collect()
        .await;

    for (wait, bound) in waits.iter().zip([1, 2, 4, 8, 16, 30]) {
        let bound = Duration::from_secs(bound);

        assert!(
            *wait >= bound / 2 && *wait <= bound,
            "{wait:?} within {bound:?}"
        );
    }

    // The waits given are the waits made: the sixth state comes after the first five, each rounded up to the
    // millisecond of tokio's timer.
    let waited = waits[..5].iter().sum::<Duration>();
    let elapsed = started.elapsed();

    assert!(
        elapsed >= waited && elapsed < waited + Duration::from_millis(5),
        "{elapsed:?} for {waited:?}"
    );
}

#[tokio::test(start_paused = true)]
async fn skips_an_item_that_does_not_read_and_goes_on() {
    let fake = Fake::leak([Connection::SendsAndStays(vec![
        Err(Error::failed(ErrorKind::Decode, "not a change")),
        Ok(change("note")),
    ])]);

    let followed = first(fake, 3).await;

    assert_eq!(followed, ["connected", "skipped decode", "item note"]);
    assert_eq!(fake.calls(), 1);
}

#[tokio::test(start_paused = true)]
async fn drops_a_connection_after_an_error_even_when_its_stream_would_go_on() {
    let fake = Fake::leak([
        Connection::SendsAndStays(vec![
            Err(Error::failed(ErrorKind::Body, "cut")),
            Ok(change("note")),
        ]),
        Connection::Sends(Vec::new()),
    ]);

    let followed = first(fake, 3).await;

    assert_eq!(
        followed,
        ["connected", "failed body, again in 1s", "connected"]
    );
}

#[tokio::test(start_paused = true)]
async fn says_a_failure_once_when_the_stream_fails_then_ends() {
    let fake = Fake::leak([
        Connection::Sends(vec![Err(Error::failed(ErrorKind::Body, "cut"))]),
        Connection::Sends(Vec::new()),
    ]);

    let followed = first(fake, 3).await;

    assert_eq!(
        followed,
        ["connected", "failed body, again in 1s", "connected"]
    );
}

#[tokio::test(start_paused = true)]
async fn reconnects_after_an_error_of_any_status_in_the_stream() {
    // An `event: problem` ends the stream with the error of its status, a 4xx as well: a token that expired is asked
    // again by the next call.
    let fake = Fake::leak([
        Connection::Sends(vec![Err(Error::unauthorized("the token expired"))]),
        Connection::Sends(vec![Err(Error::internal("the store went away"))]),
        Connection::Sends(Vec::new()),
    ]);

    let followed = first(fake, 5).await;

    assert_eq!(
        followed,
        [
            "connected",
            "failed 401, again in 1s",
            "connected",
            "failed 500, again in 2s",
            "connected"
        ]
    );
}

#[tokio::test(start_paused = true)]
async fn calls_again_after_each_kind_of_failure_that_may_pass() {
    let kinds = [
        ErrorKind::Connect,
        ErrorKind::Timeout,
        ErrorKind::Closed,
        ErrorKind::Body,
        ErrorKind::Middleware,
        // An answer that is not the route's, as a proxy's page of HTML.
        ErrorKind::Decode,
    ];
    let fake = Fake::leak(
        kinds
            .into_iter()
            .map(failed)
            .chain([Connection::Sends(Vec::new())]),
    );

    let followed = first(fake, 7).await;

    assert_eq!(
        followed,
        [
            "failed connect, again in 1s",
            "failed timeout, again in 2s",
            "failed closed, again in 4s",
            "failed body, again in 8s",
            "failed middleware, again in 16s",
            "failed decode, again in 30s",
            "connected"
        ]
    );
}

#[tokio::test(start_paused = true)]
async fn calls_again_after_a_5xx_a_408_or_a_429() {
    let fake = Fake::leak([
        answered(StatusCode::SERVICE_UNAVAILABLE),
        answered(StatusCode::REQUEST_TIMEOUT),
        answered(StatusCode::TOO_MANY_REQUESTS),
        Connection::Fails(Error::internal("the fake is down")),
        Connection::Sends(Vec::new()),
    ]);

    let followed = first(fake, 5).await;

    assert_eq!(
        followed,
        [
            "failed 503, again in 1s",
            "failed 408, again in 2s",
            "failed 429, again in 4s",
            "failed 500, again in 8s",
            "connected"
        ]
    );
}

#[tokio::test(start_paused = true)]
async fn stops_on_a_4xx_and_ends() {
    for status in [
        StatusCode::BAD_REQUEST,
        StatusCode::UNAUTHORIZED,
        StatusCode::FORBIDDEN,
        StatusCode::NOT_FOUND,
        StatusCode::GONE,
    ] {
        let fake = Fake::leak([failed(ErrorKind::Connect), answered(status)]);

        let followed: Vec<_> = followed(fake)
            .map(|followed| name(&followed))
            .collect()
            .await;

        assert_eq!(
            followed,
            [
                "failed connect, again in 1s".to_owned(),
                format!("stopped {}", status.as_str())
            ]
        );
        assert_eq!(fake.calls(), 2);
    }
}

#[tokio::test(start_paused = true)]
async fn stops_on_a_3xx_and_ends() {
    for status in [StatusCode::MOVED_PERMANENTLY, StatusCode::NOT_MODIFIED] {
        let fake = Fake::leak([Connection::Fails(Error::failed(
            ErrorKind::Status(status),
            "",
        ))]);

        let followed: Vec<_> = followed(fake)
            .map(|followed| name(&followed))
            .collect()
            .await;

        assert_eq!(followed, [format!("stopped {}", status.as_str())]);
    }
}

#[tokio::test(start_paused = true)]
async fn stops_on_a_call_the_client_cannot_send() {
    let fake = Fake::leak([failed(ErrorKind::Invalid)]);

    let followed: Vec<_> = followed(fake)
        .map(|followed| name(&followed))
        .collect()
        .await;

    assert_eq!(followed, ["stopped invalid"]);
    assert_eq!(fake.calls(), 1);
}

#[tokio::test(start_paused = true)]
async fn gives_the_errors_in_the_shape_of_the_api() {
    #[derive(Debug, Serialize, Deserialize, JsonSchema)]
    struct Message {
        message: String,
    }

    impl std::fmt::Display for Message {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            f.write_str(&self.message)
        }
    }

    // The closure is `FnMut`: it may change what it owns at each call.
    let mut errors = VecDeque::from([Error::answered(
        StatusCode::SERVICE_UNAVAILABLE,
        Message {
            message: "starting".to_owned(),
        },
    )]);
    let mut followed = Reconnect::new().without_jitter().follow(async move || {
        let Some(error) = errors.pop_front() else {
            let changes = stream::iter([Err(Error::failed(ErrorKind::Decode, "not a change"))]);

            return Ok(changes.chain(stream::pending::<quoll::Result<Change>>()));
        };

        Err(error)
    });

    let Some(Followed::Reconnecting {
        error: Some(error), ..
    }) = followed.next().await
    else {
        panic!("a failure");
    };

    assert_eq!(
        error.detail().map(|detail| detail.message.as_str()),
        Some("starting")
    );
    assert!(matches!(followed.next().await, Some(Followed::Connected)));

    let Some(Followed::Skipped(error)) = followed.next().await else {
        panic!("an item skipped");
    };
    let error: Error<Message> = error;

    assert_eq!(error.kind(), ErrorKind::Decode);
}

#[test]
fn a_followed_stream_is_send_unpin_and_static() {
    fn is_send_unpin_static<S: Stream + Send + Unpin + 'static>(_: &S) {}

    let feed = Api::client("http://localhost:8080")
        .expect("a client")
        .feed();
    let fake = Fake::leak([]);

    is_send_unpin_static(&client::follow(async move || feed.changes().await));
    is_send_unpin_static(&followed(fake));
}

#[tokio::test(start_paused = true)]
async fn a_spawned_follow_runs_on_its_own_task() {
    let fake = Fake::leak([Connection::Sends(vec![Ok(change("note"))])]);
    let (sender, mut receiver) = tokio::sync::mpsc::channel(8);

    tokio::spawn(async move {
        let mut changes = followed(fake);

        while let Some(followed) = changes.next().await {
            let Followed::Item(change) = followed else {
                continue;
            };

            if sender.send(change.kind).await.is_err() {
                return;
            }
        }
    });

    assert_eq!(receiver.recv().await.as_deref(), Some("note"));
}
