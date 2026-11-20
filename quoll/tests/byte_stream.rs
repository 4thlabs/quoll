//! The result `ByteStream`: chunks sent by the server a contract becomes as its stream gives them, and read back by
//! its client, served on a port; and read from another API's chunks as they come, of any content type.

#![cfg(all(feature = "server", feature = "client"))]

use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{StreamExt, stream};
use quoll::rest::{self, ByteStream, Error, Result};
use reqwest::StatusCode;
use reqwest::header::{ACCEPT, CACHE_CONTROL, CONTENT_TYPE};
use tokio::sync::mpsc;

rest::api! {
    api Api = "/api";

    group Output {
        GET "/chunks?count={count}" => chunks(count: u32) -> ByteStream;
        GET "/typed"                => typed() -> ByteStream;
        GET "/failing"              => failing() -> ByteStream;
        GET "/missing"              => missing() -> rest::ByteStream;
        GET "/held"                 => held() -> ByteStream;
        GET "/mistyped"             => mistyped() -> ByteStream;
    }
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

/// `first`, then chunks that never come, and a signal on `dropped`, named `route`, once the stream is dropped.
fn held_after(
    first: Vec<Vec<u8>>,
    dropped: &mpsc::UnboundedSender<&'static str>,
    route: &'static str,
) -> impl futures_util::Stream<Item = Vec<u8>> + Send + 'static {
    let signal = DropSignal {
        dropped: dropped.clone(),
        route,
    };

    stream::iter(first).chain(stream::pending().map(move |chunk: Vec<u8>| {
        let _ = &signal;
        chunk
    }))
}

/// The chunk numbered `i`.
fn chunk(i: u32) -> Vec<u8> {
    format!("chunk {i}\n").into_bytes()
}

/// The service: the held stream sends one chunk, then waits for ever, and says on `dropped` when it is dropped.
#[derive(Clone)]
struct App {
    dropped: mpsc::UnboundedSender<&'static str>,
}

impl OutputServer for App {
    async fn chunks(&self, count: u32) -> Result<ByteStream> {
        Ok(ByteStream::new(stream::iter((1..=count).map(chunk))))
    }

    async fn typed(&self) -> Result<ByteStream> {
        let chunks = stream::iter([b"caf".to_vec(), "\u{e9}".as_bytes().to_vec()]);

        Ok(ByteStream::new(chunks).with_content_type("text/plain; charset=utf-8"))
    }

    async fn failing(&self) -> Result<ByteStream> {
        let chunks = [
            Ok(chunk(1)),
            Ok(chunk(2)),
            Err(Error::internal("the process died")),
            Ok(chunk(3)),
        ];

        Ok(ByteStream::try_new(stream::iter(chunks)))
    }

    async fn missing(&self) -> Result<ByteStream> {
        Err(Error::not_found("no such output"))
    }

    async fn held(&self) -> Result<ByteStream> {
        Ok(ByteStream::new(held_after(
            vec![chunk(1)],
            &self.dropped,
            "held",
        )))
    }

    async fn mistyped(&self) -> Result<ByteStream> {
        Ok(ByteStream::new(stream::iter([chunk(1)])).with_content_type("not a type"))
    }
}

/// The api served on a port, its URL, its client, and what says when the held stream is dropped.
async fn served() -> (String, ApiClient, mpsc::UnboundedReceiver<&'static str>) {
    let (dropped, signals) = mpsc::unbounded_channel();
    let url = quoll::test::spawn(Api::router(ApiServices {
        output: App { dropped },
    }))
    .await;
    let api = Api::client(&url).expect("the url is the api's");

    (url, api, signals)
}

/// Every chunk of `stream`, until it ends.
async fn all(stream: impl futures_util::Stream<Item = Result<Vec<u8>>>) -> Vec<Result<Vec<u8>>> {
    stream.collect().await
}

/// The bytes of `chunks`, which are all chunks, end to end.
fn joined(chunks: Vec<Result<Vec<u8>>>) -> Vec<u8> {
    chunks
        .into_iter()
        .flat_map(|chunk| chunk.expect("a chunk"))
        .collect()
}

/// Waits for the server to say which held stream it dropped: a stream it never drops fails the test, rather than
/// hold it for ever.
async fn dropped(signals: &mut mpsc::UnboundedReceiver<&'static str>) -> &'static str {
    tokio::time::timeout(Duration::from_secs(10), signals.recv())
        .await
        .expect("the stream is dropped")
        .expect("the sender is still up")
}

#[tokio::test]
async fn chunks_are_sent_as_they_are_and_read_back() {
    let (url, api, _) = served().await;

    let response = reqwest::get(format!("{url}/api/chunks?count=2"))
        .await
        .expect("the server answers");
    let headers = response.headers().clone();

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(headers[CONTENT_TYPE], "application/octet-stream");
    assert_eq!(headers["x-content-type-options"], "nosniff");
    assert_eq!(headers[CACHE_CONTROL], "no-cache");
    assert_eq!(headers["x-accel-buffering"], "no");
    assert_eq!(
        response.text().await.expect("the body reads"),
        "chunk 1\nchunk 2\n"
    );

    let output = api.output().chunks(3).await.expect("chunks");

    assert_eq!(output.content_type(), "application/octet-stream");
    assert_eq!(joined(all(output).await), b"chunk 1\nchunk 2\nchunk 3\n");
}

#[tokio::test]
async fn the_content_type_given_is_sent_and_read_back() {
    let (_, api, _) = served().await;

    let output = api.output().typed().await.expect("chunks");

    assert_eq!(output.content_type(), "text/plain; charset=utf-8");
    assert!(output.has_type("text/plain"));
    assert_eq!(joined(all(output).await), "café".as_bytes());
}

#[tokio::test]
async fn an_empty_stream_sends_nothing_and_ends() {
    let (_, api, _) = served().await;

    let output = api.output().chunks(0).await.expect("chunks");

    assert!(all(output).await.is_empty());
}

#[tokio::test]
async fn many_chunks_come_back_whole_and_in_order() {
    let (_, api, _) = served().await;
    let expected: Vec<u8> = (1..=20_000).flat_map(chunk).collect();

    let output = api.output().chunks(20_000).await.expect("chunks");

    assert!(joined(all(output).await) == expected);
}

#[tokio::test]
async fn an_error_before_the_stream_is_a_problem_with_its_status() {
    let (_, api, _) = served().await;

    let error = api.output().missing().await.expect_err("not found");

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(
        error.problem().and_then(rest::Problem::detail),
        Some("no such output")
    );
}

#[tokio::test]
async fn a_content_type_that_is_not_a_media_type_answers_500() {
    let (_, api, _) = served().await;

    let error = api.output().mistyped().await.expect_err("a 500");

    assert_eq!(error.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
}

#[tokio::test]
async fn a_chunk_that_fails_cuts_the_body_short_with_an_error() {
    let (_, api, _) = served().await;

    // The call itself fails when the server had written nothing before the error.
    let mut chunks = match api.output().failing().await {
        Ok(output) => all(output).await,
        Err(error) => vec![Err(error)],
    };

    // An error rather than an end. The chunks sent just before it may not arrive, but never the one after.
    let error = chunks.pop().expect("an error").expect_err("an error");
    let before = String::from_utf8(joined(chunks)).expect("text");

    assert!("chunk 1\nchunk 2\n".starts_with(&before), "{before:?}");
    assert_eq!(error.status(), None, "{error}");
    assert!(
        [
            "the call `output_failing` got an answer whose body cannot be read",
            "the call `output_failing` got no response",
        ]
        .contains(&error.to_string().as_str()),
        "{error}"
    );
}

#[tokio::test]
async fn dropping_the_clients_stream_drops_the_servers() {
    let (_, api, mut signals) = served().await;

    let mut output = api.output().held().await.expect("chunks");
    let first = output
        .next()
        .await
        .expect("a first chunk")
        .expect("a chunk");

    assert_eq!(first, chunk(1));
    drop(output);
    assert_eq!(dropped(&mut signals).await, "held");
}

/// Another API, written without the contract: it answers with the chunks the query names, each sent on its own, of
/// the content type it names, or none; and an endless feed, which says on `dropped` when its body is dropped.
fn third_party(
    accepts: Arc<Mutex<Vec<String>>>,
    dropped: mpsc::UnboundedSender<&'static str>,
) -> axum::Router {
    use axum::body::Body;
    use axum::http::{HeaderMap, Uri};
    use axum::response::{IntoResponse, Response};
    use axum::routing::get;

    fn answer(
        status: StatusCode,
        content_type: Option<&'static str>,
        chunks: &[&'static [u8]],
    ) -> Response {
        let chunks: Vec<Result<&'static [u8], std::convert::Infallible>> =
            chunks.iter().copied().map(Ok).collect();
        let mut response = (status, Body::from_stream(stream::iter(chunks))).into_response();

        if let Some(content_type) = content_type {
            response.headers_mut().insert(
                CONTENT_TYPE,
                content_type.parse().expect("a header's value"),
            );
        }

        response
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

    axum::Router::new()
        .route(
            "/api/chunks",
            get(move |uri: Uri, headers: HeaderMap| async move {
                record(&headers);

                match uri.query().unwrap_or_default() {
                    // Docker's frames of a container's output, cut anywhere.
                    "count=1" => answer(
                        StatusCode::OK,
                        Some("application/vnd.docker.multiplexed-stream"),
                        &[
                            b"\x01\x00\x00",
                            b"\x00\x00\x00\x00\x03ab",
                            b"c\x02\x00\x00\x00\x00\x00\x00\x01",
                            b"d",
                        ],
                    ),
                    // A page, of another success status: bytes of any type are the result.
                    "count=2" => answer(
                        StatusCode::PARTIAL_CONTENT,
                        Some("text/html"),
                        &[b"<p>a ", b"page"],
                    ),
                    _ => answer(StatusCode::OK, None, &[b"no ", b"type"]),
                }
            }),
        )
        .route(
            "/api/held",
            get(move || async move {
                let body = held_after(vec![b"first".to_vec()], &dropped, "feed")
                    .map(Ok::<_, std::convert::Infallible>);

                ([(CONTENT_TYPE, "text/plain")], Body::from_stream(body)).into_response()
            }),
        )
}

/// A client of another API, the `Accept` of each request it got, and what says when its feed is dropped.
async fn third_party_client() -> (
    ApiClient,
    Arc<Mutex<Vec<String>>>,
    mpsc::UnboundedReceiver<&'static str>,
) {
    let accepts = Arc::default();
    let (dropped, signals) = mpsc::unbounded_channel();
    let url = quoll::test::spawn(third_party(Arc::clone(&accepts), dropped)).await;
    let api = Api::client(&url).expect("the url is the api's");

    (api, accepts, signals)
}

#[tokio::test]
async fn chunks_cut_anywhere_are_read_whole_whatever_their_type() {
    let (api, accepts, _) = third_party_client().await;

    let output = api.output().chunks(1).await.expect("chunks");

    assert!(output.has_type("application/vnd.docker.multiplexed-stream"));
    assert_eq!(
        joined(all(output).await),
        b"\x01\x00\x00\x00\x00\x00\x00\x03abc\x02\x00\x00\x00\x00\x00\x00\x01d"
    );
    assert_eq!(
        *accepts.lock().expect("no test panics holding the lock"),
        ["*/*"]
    );
}

#[tokio::test]
async fn another_success_of_another_type_is_read() {
    let (api, _, _) = third_party_client().await;

    let page = api.output().chunks(2).await.expect("a page");

    assert_eq!(page.content_type(), "text/html");
    assert_eq!(joined(all(page).await), b"<p>a page");
}

#[tokio::test]
async fn chunks_with_no_content_type_are_octet_stream() {
    let (api, _, _) = third_party_client().await;

    let output = api.output().chunks(3).await.expect("chunks");

    assert_eq!(output.content_type(), "application/octet-stream");
    assert_eq!(joined(all(output).await), b"no type");
}

#[tokio::test]
async fn an_endless_feed_is_read_as_it_comes_then_dropped() {
    let (api, _, mut signals) = third_party_client().await;

    let mut feed = api.output().held().await.expect("a feed");
    let first = tokio::time::timeout(Duration::from_secs(10), feed.next())
        .await
        .expect("the first chunk comes before the end")
        .expect("a first chunk")
        .expect("a chunk");

    assert_eq!(first, b"first");
    drop(feed);
    assert_eq!(dropped(&mut signals).await, "feed");
}
