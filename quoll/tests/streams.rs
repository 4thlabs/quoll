//! The results a route streams, `Events<T>` and `JsonLines<T>`: sent by the server a contract becomes, as its stream
//! gives them, and read back by its client, served on a port; and read from another API's chunks as they come.

#![cfg(all(feature = "server", feature = "client"))]

use std::error::Error as _;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{StreamExt, stream};
use quoll::rest::{self, Error, Events, JsonLines, Result};
use reqwest::StatusCode;
use reqwest::header::{ACCEPT, CACHE_CONTROL, CONTENT_TYPE};
use serde::{Deserialize, Serialize};
use tokio::sync::mpsc;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
struct Change {
    id: u32,
    text: String,
}

rest::api! {
    api Api = "/api";

    group Feeds {
        GET "/events?count={count}" => events(count: u32) -> Events<Change>;
        GET "/lines?count={count}"  => lines(count: u32) -> JsonLines<Change>;
        GET "/failing/events"       => failing_events() -> Events<Change>;
        GET "/failing/lines"        => failing_lines() -> rest::JsonLines<Change>;
        GET "/gone/events"          => gone_events() -> Events<Change>;
        GET "/missing/events"       => missing_events() -> rest::Events<Change>;
        GET "/missing/lines"        => missing_lines() -> JsonLines<Change>;
        GET "/held/events"          => held_events() -> Events<Change>;
        GET "/held/lines"           => held_lines() -> JsonLines<Change>;
    }
}

/// The service: `held` routes send one change, then wait for ever, and say on `dropped` when their stream is dropped.
#[derive(Clone)]
struct App {
    dropped: mpsc::UnboundedSender<&'static str>,
}

fn change(id: u32) -> Change {
    Change {
        id,
        text: format!("line\n{id} \u{e9}"),
    }
}

/// `count` changes, numbered from 1.
fn changes(count: u32) -> impl futures_util::Stream<Item = Change> + Send + 'static {
    stream::iter((1..=count).map(change))
}

/// Says on `dropped` that the stream holding it was dropped.
struct DropSignal {
    dropped: mpsc::UnboundedSender<&'static str>,
    route: &'static str,
}

impl Drop for DropSignal {
    fn drop(&mut self) {
        // The test may be gone already: nothing is left to tell then.
        let _ = self.dropped.send(self.route);
    }
}

/// One change, then a stream that never ends, and says on `dropped` when it is dropped.
fn held(
    dropped: &mpsc::UnboundedSender<&'static str>,
    route: &'static str,
) -> impl futures_util::Stream<Item = Change> + Send + 'static {
    let signal = DropSignal {
        dropped: dropped.clone(),
        route,
    };

    stream::once(async { change(1) }).chain(stream::pending().map(move |change: Change| {
        let _ = &signal;
        change
    }))
}

/// Two changes, then an error, and a change that is never sent.
fn failing() -> impl futures_util::Stream<Item = Result<Change>> + Send + 'static {
    stream::iter([
        Ok(change(1)),
        Ok(change(2)),
        Err(Error::internal("the store went away")),
        Ok(change(3)),
    ])
}

impl FeedsServer for App {
    async fn events(&self, count: u32) -> Result<Events<Change>> {
        Ok(Events::new(changes(count)))
    }

    async fn lines(&self, count: u32) -> Result<JsonLines<Change>> {
        Ok(JsonLines::new(changes(count)))
    }

    async fn failing_events(&self) -> Result<Events<Change>> {
        Ok(Events::try_new(failing()))
    }

    async fn failing_lines(&self) -> Result<JsonLines<Change>> {
        Ok(JsonLines::try_new(failing()))
    }

    async fn gone_events(&self) -> Result<Events<Change>> {
        let changes = [Ok(change(1)), Err(Error::not_found("the note is gone"))];

        Ok(Events::try_new(stream::iter(changes)))
    }

    async fn missing_events(&self) -> Result<Events<Change>> {
        Err(Error::not_found("no such feed"))
    }

    async fn missing_lines(&self) -> Result<JsonLines<Change>> {
        Err(Error::not_found("no such feed"))
    }

    async fn held_events(&self) -> Result<Events<Change>> {
        Ok(Events::new(held(&self.dropped, "events")))
    }

    async fn held_lines(&self) -> Result<JsonLines<Change>> {
        Ok(JsonLines::new(held(&self.dropped, "lines")))
    }
}

/// The api served on a port, its URL, its client, and what says when a held stream is dropped.
async fn served() -> (String, ApiClient, mpsc::UnboundedReceiver<&'static str>) {
    let (dropped, signals) = mpsc::unbounded_channel();
    let url = rest::test::spawn(Api::router(ApiServices {
        feeds: App { dropped },
    }))
    .await;
    let api = Api::client(&url).expect("the url is the api's");

    (url, api, signals)
}

/// Every item of `stream`, until it ends.
async fn all<T>(stream: impl futures_util::Stream<Item = Result<T>>) -> Vec<Result<T>> {
    stream.collect().await
}

/// The values of `items`, which are all values.
fn values<T: std::fmt::Debug>(items: Vec<Result<T>>) -> Vec<T> {
    items
        .into_iter()
        .map(|item| item.expect("a value"))
        .collect()
}

/// The status, the headers and the whole body of a GET sent as it is.
async fn raw(url: &str) -> (StatusCode, reqwest::header::HeaderMap, String) {
    let response = reqwest::get(url).await.expect("the server answers");
    let status = response.status();
    let headers = response.headers().clone();
    let body = response.text().await.expect("the body reads");

    (status, headers, body)
}

#[tokio::test]
async fn events_are_sent_as_their_json_data_and_read_back() {
    let (url, api, _) = served().await;

    let (status, headers, body) = raw(&format!("{url}/api/events?count=2")).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(headers[CONTENT_TYPE], "text/event-stream");
    assert_eq!(headers[CACHE_CONTROL], "no-cache");
    assert_eq!(headers["x-accel-buffering"], "no");
    assert_eq!(
        body,
        "data: {\"id\":1,\"text\":\"line\\n1 é\"}\n\ndata: {\"id\":2,\"text\":\"line\\n2 é\"}\n\n"
    );

    let events = api.feeds().events(3).await.expect("events");

    assert_eq!(values(all(events).await), [change(1), change(2), change(3)]);
}

#[tokio::test]
async fn json_lines_are_sent_one_value_a_line_and_read_back() {
    let (url, api, _) = served().await;

    let (status, headers, body) = raw(&format!("{url}/api/lines?count=2")).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(headers[CONTENT_TYPE], "application/jsonl");
    assert_eq!(headers[CACHE_CONTROL], "no-cache");
    assert_eq!(headers["x-accel-buffering"], "no");
    assert_eq!(
        body,
        "{\"id\":1,\"text\":\"line\\n1 é\"}\n{\"id\":2,\"text\":\"line\\n2 é\"}\n"
    );

    let lines = api.feeds().lines(3).await.expect("lines");

    assert_eq!(values(all(lines).await), [change(1), change(2), change(3)]);
}

#[tokio::test]
async fn an_empty_stream_sends_nothing_and_ends() {
    let (url, api, _) = served().await;

    assert_eq!(raw(&format!("{url}/api/events?count=0")).await.2, "");
    assert_eq!(raw(&format!("{url}/api/lines?count=0")).await.2, "");

    let events = api.feeds().events(0).await.expect("events");
    let lines = api.feeds().lines(0).await.expect("lines");

    assert!(all(events).await.is_empty());
    assert!(all(lines).await.is_empty());
}

#[tokio::test]
async fn many_items_come_back_whole_and_in_order() {
    let (_, api, _) = served().await;
    let expected: Vec<Change> = (1..=20_000).map(change).collect();

    let events = api.feeds().events(20_000).await.expect("events");
    let lines = api.feeds().lines(20_000).await.expect("lines");

    assert!(values(all(events).await) == expected);
    assert!(values(all(lines).await) == expected);
}

#[tokio::test]
async fn an_error_before_the_stream_is_a_problem_with_its_status() {
    let (_, api, _) = served().await;

    let events = api.feeds().missing_events().await.expect_err("not found");
    let lines = api.feeds().missing_lines().await.expect_err("not found");

    for error in [events, lines] {
        assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
        assert_eq!(
            error.problem().and_then(rest::Problem::detail),
            Some("no such feed")
        );
    }
}

/// Asserts that `error` has no status, says `message` of the call `operation`, and that its source says `why`.
fn no_status(error: &Error, operation: &str, message: &str, why: &str) {
    assert_eq!(error.status(), None, "{error}");
    assert_eq!(
        error.to_string(),
        format!("the call `{operation}` {message}")
    );

    let source = error.source().expect("the error says why").to_string();
    assert!(source.contains(why), "{source}");
}

#[tokio::test]
async fn an_item_that_fails_ends_events_with_its_problem_after_every_value() {
    let (url, api, _) = served().await;

    // The problem of an internal error, with no detail, as the last event of a body that ends as any other.
    let (status, _, body) = raw(&format!("{url}/api/failing/events")).await;
    assert_eq!(status, StatusCode::OK);
    assert!(
        body.ends_with(
            "\n\nevent: problem\ndata: {\"type\":\"about:blank\",\"title\":\"Internal Server Error\",\"status\":500}\n\n"
        ),
        "{body}"
    );

    let mut events = api.feeds().failing_events().await.expect("events");
    let mut next = async || events.next().await;

    assert_eq!(next().await.expect("a value").expect("1"), change(1));
    assert_eq!(next().await.expect("a value").expect("2"), change(2));

    let error = next().await.expect("the problem").expect_err("an error");

    assert_eq!(error.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
    assert_eq!(error.problem().and_then(rest::Problem::detail), None);
    assert!(next().await.is_none());

    // A problem with a detail gives it.
    let items = all(api.feeds().gone_events().await.expect("events")).await;

    assert_eq!(items.len(), 2);
    assert_eq!(items[0].as_ref().expect("a value"), &change(1));

    let error = items[1].as_ref().expect_err("the problem");

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(
        error.problem().and_then(rest::Problem::detail),
        Some("the note is gone")
    );
}

#[tokio::test]
async fn an_item_that_fails_cuts_json_lines_short_with_an_error() {
    let (_, api, _) = served().await;

    // The call itself fails when the server had written nothing before the error.
    let mut items = match api.feeds().failing_lines().await {
        Ok(lines) => all(lines).await,
        Err(error) => vec![Err(error)],
    };

    // An error rather than an end. The lines sent just before it may not arrive, but never the one after.
    let error = items.pop().expect("an error").expect_err("an error");
    let before = values(items);

    assert!(before.len() <= 2, "{before:?}");
    assert_eq!(before, [change(1), change(2)][..before.len()]);
    assert_eq!(error.status(), None, "{error}");
    assert!(
        [
            "the call `feeds_failing_lines` got an answer whose body cannot be read",
            "the call `feeds_failing_lines` got no response",
        ]
        .contains(&error.to_string().as_str()),
        "{error}"
    );
}

/// Waits for the server to say which held stream it dropped: a stream it never drops fails the test, rather than
/// hold it for ever.
async fn dropped(signals: &mut mpsc::UnboundedReceiver<&'static str>) -> &'static str {
    tokio::time::timeout(Duration::from_secs(10), signals.recv())
        .await
        .expect("the server drops the stream")
        .expect("the server is still up")
}

#[tokio::test]
async fn dropping_the_clients_stream_drops_the_servers() {
    let (_, api, mut signals) = served().await;

    let mut events = api.feeds().held_events().await.expect("events");
    let first = events
        .next()
        .await
        .expect("a first event")
        .expect("a value");
    assert_eq!(first, change(1));
    drop(events);
    assert_eq!(dropped(&mut signals).await, "events");

    let mut lines = api.feeds().held_lines().await.expect("lines");
    let first = lines.next().await.expect("a first line").expect("a value");
    assert_eq!(first, change(1));
    drop(lines);
    assert_eq!(dropped(&mut signals).await, "lines");
}

/// Another API, written without the contract: it answers the streams' routes with the chunks the query names, each
/// sent on its own, and the content type of a stream, or another one.
fn third_party(accepts: Arc<Mutex<Vec<String>>>) -> axum::Router {
    use axum::body::Body;
    use axum::http::{HeaderMap, Uri};
    use axum::response::{IntoResponse, Response};
    use axum::routing::get;

    fn answer(content_type: &'static str, chunks: &[&'static str]) -> Response {
        let chunks: Vec<Result<&'static str, std::convert::Infallible>> =
            chunks.iter().copied().map(Ok).collect();

        (
            [(CONTENT_TYPE, content_type)],
            Body::from_stream(stream::iter(chunks)),
        )
            .into_response()
    }

    let record = move |headers: &HeaderMap| {
        let accept = headers
            .get(ACCEPT)
            .map(|value| value.to_str().expect("an Accept is text").to_owned())
            .unwrap_or_default();

        accepts
            .lock()
            .expect("no test panics holding the lock")
            .push(accept);
    };
    let record_lines = record.clone();

    axum::Router::new()
        .route(
            "/api/events",
            get(move |uri: Uri, headers: HeaderMap| async move {
                record(&headers);

                match uri.query().unwrap_or_default() {
                    // An event cut anywhere, lines ended in every way, comments, other fields and other events.
                    "count=1" => answer(
                        "text/event-stream; charset=utf-8",
                        &[
                            "\u{feff}: a comment\r\n\r\nid: 7\r\nretry: 1000\r\nda",
                            "ta: {\"id\":1,\r\ndata: \"text\":\"a\"}\r",
                            "\n\r\nevent: ping\ndata: {\"id\":99,\"text\":\"ping\"}\n\nevent: ping\n\ndata:{\"id\":2,\"text\":",
                            "\"caf\u{e9}\"}\r\rdata: {\"id\":3,\"text\":\"\"}\n\n",
                            "data: {\"id\":4,\"text\":\"never ended\"}\n",
                        ],
                    ),
                    // An event that does not read as a change, between two that do.
                    "count=2" => answer(
                        "text/event-stream",
                        &[
                            "data: {\"id\":1,\"text\":\"a\"}\n\n",
                            "data: {\"id\":\"two\"}\n\n",
                            "data: {\"id\":3,\"text\":\"c\"}\n\n",
                        ],
                    ),
                    // An API that names every event, as Anthropic's does.
                    "count=4" => answer(
                        "text/event-stream",
                        &[
                            "event: message_start\ndata: {\"id\":1,\"text\":\"start\"}\n\n",
                            "event: content_block_delta\ndata: {\"id\":2,\"text\":\"delta\"}\n\n",
                            "event: ping\n\n",
                            "event: message_stop\ndata: {\"id\":3,\"text\":\"stop\"}\n\n",
                        ],
                    ),
                    // A problem of another API's, after which nothing is read.
                    "count=5" => answer(
                        "text/event-stream",
                        &[
                            "data: {\"id\":1,\"text\":\"a\"}\n\n",
                            "event: problem\ndata: {\"type\":\"https://example.com/down\",\"status\":503,\"detail\":\"down\"}\n\n",
                            "data: {\"id\":2,\"text\":\"b\"}\n\n",
                        ],
                    ),
                    _ => answer("application/json", &["[{\"id\":1,\"text\":\"a\"}]"]),
                }
            }),
        )
        .route(
            "/api/lines",
            get(move |uri: Uri, headers: HeaderMap| async move {
                record_lines(&headers);

                match uri.query().unwrap_or_default() {
                    // A line cut anywhere, `\r\n`, empty lines, and a last line with no line break.
                    "count=1" => answer(
                        "application/jsonl",
                        &[
                            "\n{\"id\":1,\"te",
                            "xt\":\"a\"}\r\n\r\n",
                            "  \n{\"id\":2,\"text\":\"caf\u{e9}\"}\n{\"id\":3,",
                            "\"text\":\"no line break\"}",
                        ],
                    ),
                    "count=2" => answer(
                        "Application/JSONL",
                        &[
                            "{\"id\":1,\"text\":\"a\"}\n",
                            "not json\n",
                            "{\"id\":3,\"text\":\"c\"}\n",
                        ],
                    ),
                    // The names some APIs give JSON Lines.
                    "count=4" => answer("application/x-ndjson", &["{\"id\":4,\"text\":\"d\"}\n"]),
                    "count=5" => answer(
                        "Application/X-JSONLines; charset=utf-8",
                        &["{\"id\":5,\"text\":\"e\"}\n"],
                    ),
                    _ => answer("application/json", &["[{\"id\":1,\"text\":\"a\"}]"]),
                }
            }),
        )
}

/// A client of another API, and the `Accept` of each request it got.
async fn third_party_client() -> (ApiClient, Arc<Mutex<Vec<String>>>) {
    let accepts = Arc::default();
    let url = rest::test::spawn(third_party(Arc::clone(&accepts))).await;
    let api = Api::client(&url).expect("the url is the api's");

    (api, accepts)
}

fn changed(id: u32, text: &str) -> Change {
    Change {
        id,
        text: text.into(),
    }
}

#[tokio::test]
async fn events_are_read_from_chunks_cut_anywhere_as_an_event_source_reads_them() {
    let (api, accepts) = third_party_client().await;

    let events = api.feeds().events(1).await.expect("events");

    // A named event is read, one with no data is not, and the event the body ends before its blank line is dropped.
    assert_eq!(
        values(all(events).await),
        [
            changed(1, "a"),
            changed(99, "ping"),
            changed(2, "café"),
            changed(3, "")
        ]
    );
    assert_eq!(
        *accepts.lock().expect("no test panics holding the lock"),
        ["text/event-stream"]
    );
}

#[tokio::test]
async fn events_are_read_whatever_their_name() {
    let (api, _) = third_party_client().await;

    let events = api.feeds().events(4).await.expect("events");

    assert_eq!(
        values(all(events).await),
        [changed(1, "start"), changed(2, "delta"), changed(3, "stop")]
    );
}

#[tokio::test]
async fn a_problem_of_another_api_is_its_error_and_ends_the_stream() {
    let (api, _) = third_party_client().await;

    let mut items = all(api.feeds().events(5).await.expect("events")).await;

    assert_eq!(items.len(), 2);

    let error = items.pop().expect("the problem").expect_err("an error");

    assert_eq!(values(items), [changed(1, "a")]);
    assert_eq!(error.status(), Some(StatusCode::SERVICE_UNAVAILABLE));
    assert_eq!(
        error.problem().map(rest::Problem::kind),
        Some("https://example.com/down")
    );
    assert_eq!(
        error.problem().and_then(rest::Problem::detail),
        Some("down")
    );
}

#[tokio::test]
async fn json_lines_are_read_from_chunks_cut_anywhere() {
    let (api, accepts) = third_party_client().await;

    let lines = api.feeds().lines(1).await.expect("lines");

    assert_eq!(
        values(all(lines).await),
        [
            changed(1, "a"),
            changed(2, "café"),
            changed(3, "no line break")
        ]
    );
    assert_eq!(
        *accepts.lock().expect("no test panics holding the lock"),
        ["application/jsonl, application/x-ndjson;q=0.9, application/x-jsonlines;q=0.9"]
    );
}

#[tokio::test]
async fn json_lines_are_read_by_the_names_other_apis_give_them() {
    let (api, _) = third_party_client().await;

    let ndjson = api.feeds().lines(4).await.expect("application/x-ndjson");
    let jsonlines = api.feeds().lines(5).await.expect("application/x-jsonlines");

    assert_eq!(values(all(ndjson).await), [changed(4, "d")]);
    assert_eq!(values(all(jsonlines).await), [changed(5, "e")]);
}

#[tokio::test]
async fn an_item_that_does_not_read_is_an_error_and_the_stream_goes_on() {
    let (api, _) = third_party_client().await;

    let events = all(api.feeds().events(2).await.expect("events")).await;
    let lines = all(api.feeds().lines(2).await.expect("lines")).await;

    for (mut items, operation) in [(events, "feeds_events"), (lines, "feeds_lines")] {
        assert_eq!(items.len(), 3, "{operation}");

        let error = items.remove(1).expect_err("not a change");

        no_status(
            &error,
            operation,
            "got an item that does not read as its type",
            if operation == "feeds_events" {
                "invalid type"
            } else {
                "expected ident"
            },
        );
        assert_eq!(values(items), [changed(1, "a"), changed(3, "c")]);
    }
}

#[tokio::test]
async fn a_stream_of_another_content_type_is_an_error_with_no_status() {
    let (api, _) = third_party_client().await;

    let events = api.feeds().events(3).await.expect_err("JSON");
    let lines = api.feeds().lines(3).await.expect_err("JSON");

    no_status(
        &events,
        "feeds_events",
        "got an answer that is not its result",
        "its content type is not `text/event-stream`",
    );
    no_status(
        &lines,
        "feeds_lines",
        "got an answer that is not its result",
        "its content type is not `application/jsonl`, `application/x-ndjson` or `application/x-jsonlines`",
    );
}

/// Another API, written without the contract, whose answers never end: each sends what the query names, then waits
/// for ever, and says on `dropped` when its body is dropped, as its connection closes.
fn endless(dropped: mpsc::UnboundedSender<&'static str>) -> axum::Router {
    use axum::body::Body;
    use axum::http::Uri;
    use axum::response::IntoResponse;
    use axum::routing::get;

    let answer = move |route: &'static str, content_type: &'static str, chunks: Vec<Vec<u8>>| {
        let signal = DropSignal {
            dropped: dropped.clone(),
            route,
        };
        let never = stream::pending().map(move |chunk: Vec<u8>| {
            let _ = &signal;
            chunk
        });
        let body = stream::iter(chunks)
            .chain(never)
            .map(Ok::<_, std::convert::Infallible>);

        ([(CONTENT_TYPE, content_type)], Body::from_stream(body)).into_response()
    };
    let lines = answer.clone();
    // A line of more than 1 MiB, in chunks of 64 KiB, and never ended.
    let long = |start: &str| {
        let mut chunks = vec![start.as_bytes().to_vec()];

        chunks.extend((0..17).map(|_| vec![b'a'; 64 * 1024]));
        chunks
    };

    axum::Router::new()
        .route(
            "/api/events",
            get(move |uri: Uri| async move {
                match uri.query().unwrap_or_default() {
                    "count=1" => answer("html", "text/html", vec![b"<p>a page".to_vec()]),
                    _ => answer("long event", "text/event-stream", long("data: \"")),
                }
            }),
        )
        .route(
            "/api/lines",
            get(move |uri: Uri| async move {
                match uri.query().unwrap_or_default() {
                    "count=1" => lines("html lines", "text/html", vec![b"<p>a page".to_vec()]),
                    _ => lines("long line", "application/jsonl", long("\"")),
                }
            }),
        )
}

/// A client of an endless API, and what says when it drops a body.
async fn endless_client() -> (ApiClient, mpsc::UnboundedReceiver<&'static str>) {
    let (dropped, signals) = mpsc::unbounded_channel();
    let url = rest::test::spawn(endless(dropped)).await;
    let api = Api::client(&url).expect("the url is the api's");

    (api, signals)
}

/// `call`, which fails the test rather than wait for ever.
async fn soon<T>(call: impl Future<Output = T>) -> T {
    tokio::time::timeout(Duration::from_secs(10), call)
        .await
        .expect("the call does not wait for the body to end")
}

#[tokio::test]
async fn a_stream_of_another_content_type_is_dropped_at_once() {
    let (api, mut signals) = endless_client().await;

    let events = soon(api.feeds().events(1)).await.expect_err("HTML");
    no_status(
        &events,
        "feeds_events",
        "got an answer that is not its result",
        "its content type is not `text/event-stream`",
    );
    assert_eq!(dropped(&mut signals).await, "html");

    let lines = soon(api.feeds().lines(1)).await.expect_err("HTML");
    no_status(
        &lines,
        "feeds_lines",
        "got an answer that is not its result",
        "its content type is not `application/jsonl`",
    );
    assert_eq!(dropped(&mut signals).await, "html lines");
}

#[tokio::test]
async fn an_item_longer_than_1_mib_is_an_error_and_ends_the_stream() {
    let (api, mut signals) = endless_client().await;

    let events = soon(all(api.feeds().events(2).await.expect("events"))).await;
    let lines = soon(all(api.feeds().lines(2).await.expect("lines"))).await;

    for (mut items, operation) in [(events, "feeds_events"), (lines, "feeds_lines")] {
        let error = items.pop().expect("an error").expect_err("too long");

        assert!(items.is_empty(), "{operation}");
        no_status(
            &error,
            operation,
            "got an item longer than the client reads",
            "more than 1 MiB",
        );
    }

    let mut both = [dropped(&mut signals).await, dropped(&mut signals).await];

    both.sort_unstable();
    assert_eq!(both, ["long event", "long line"]);
}
