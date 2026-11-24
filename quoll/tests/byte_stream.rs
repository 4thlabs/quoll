//! The result `ByteStream`: chunks sent by the server a contract becomes as its stream gives them, and read back by
//! its client, served on a port, with no length or with one; and read from another API's chunks as they come, of any
//! content type.

#![cfg(all(feature = "server", feature = "client"))]

use std::sync::{Arc, Mutex};
use std::task::Poll;
use std::time::Duration;

use futures_util::{StreamExt, stream};
use quoll::{ByteStream, Error, Response, Result};
use reqwest::StatusCode;
use reqwest::header::{
    ACCEPT, CACHE_CONTROL, CONTENT_ENCODING, CONTENT_LENGTH, CONTENT_TYPE, TRANSFER_ENCODING,
};
use tokio::sync::mpsc;

mod support {
    pub(crate) mod log;
}

quoll::api! {
    api Api = "/api";

    group Output {
        GET "/chunks?count={count}" => chunks(count: u32) -> ByteStream;
        GET "/typed"                => typed() -> ByteStream;
        GET "/failing"              => failing() -> ByteStream;
        GET "/missing"              => missing() -> quoll::ByteStream;
        GET "/held"                 => held() -> ByteStream;
        GET "/mistyped"             => mistyped() -> ByteStream;
        GET "/sized?count={count}&length={length}" => sized(count: u32, length: u64) -> ByteStream;
        GET "/sized_failing"        => sized_failing() -> ByteStream;
        GET "/empty_sized"          => empty_sized() -> ByteStream;
    }
}

quoll::api! {
    api Front = "/front";

    group Proxy {
        GET "/forwarded" => forwarded() -> ByteStream;
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
    async fn chunks(&self, count: u32) -> Result<Response<ByteStream>> {
        Ok(Response::new(ByteStream::new(stream::iter(
            (1..=count).map(chunk),
        ))))
    }

    async fn typed(&self) -> Result<Response<ByteStream>> {
        let chunks = stream::iter([b"caf".to_vec(), "\u{e9}".as_bytes().to_vec()]);

        Ok(Response::new(
            ByteStream::new(chunks).with_content_type("text/plain; charset=utf-8"),
        ))
    }

    async fn failing(&self) -> Result<Response<ByteStream>> {
        let chunks = [
            Ok(chunk(1)),
            Ok(chunk(2)),
            Err(Error::internal("the process died")),
            Ok(chunk(3)),
        ];

        Ok(Response::new(ByteStream::try_new(stream::iter(chunks))))
    }

    async fn missing(&self) -> Result<Response<ByteStream>> {
        Err(Error::not_found("no such output"))
    }

    async fn held(&self) -> Result<Response<ByteStream>> {
        Ok(Response::new(ByteStream::new(held_after(
            vec![chunk(1)],
            &self.dropped,
            "held",
        ))))
    }

    async fn mistyped(&self) -> Result<Response<ByteStream>> {
        Ok(Response::new(
            ByteStream::new(stream::iter([chunk(1)])).with_content_type("not a type"),
        ))
    }

    async fn sized(&self, count: u32, length: u64) -> Result<Response<ByteStream>> {
        let chunks = stream::iter((1..=count).map(chunk));

        Ok(Response::new(
            ByteStream::new(chunks)
                .with_length(length)
                .with_content_type("text/plain"),
        ))
    }

    async fn sized_failing(&self) -> Result<Response<ByteStream>> {
        let chunks = [Ok(chunk(1)), Err(Error::internal("the disk went away"))];

        Ok(Response::new(
            ByteStream::try_new(stream::iter(chunks)).with_length(16),
        ))
    }

    async fn empty_sized(&self) -> Result<Response<ByteStream>> {
        let signal = DropSignal {
            dropped: self.dropped.clone(),
            route: "empty_sized",
        };
        let polled = self.dropped.clone();
        let chunks = stream::poll_fn(move |_| {
            let _ = &signal;
            let _ = polled.send("empty_sized polled");

            Poll::Ready(Some(b"one too many".to_vec()))
        });

        Ok(Response::new(ByteStream::new(chunks).with_length(0)))
    }
}

/// A server in front of the api, which forwards its sized stream once it has read the first chunk, and keeps how many
/// bytes it read: the connection may give the chunks of the api together.
struct Forward {
    api: ApiClient,
    read: Arc<Mutex<Option<usize>>>,
}

impl ProxyServer for Forward {
    async fn forwarded(&self) -> Result<Response<ByteStream>> {
        let mut output = self.api.output().sized(3, 24).await?;
        let first = output.next().await.transpose()?.expect("a first chunk");

        *self.read.lock().expect("no test panics holding the lock") = Some(first.len());
        Ok(Response::new(output))
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

/// The chunks the client reads of `call`, the error that ends them, and what the server logged: a call that fails
/// when the server had sent nothing before its error is that error.
async fn cut_short(
    call: impl std::future::IntoFuture<Output = Result<ByteStream>>,
) -> (Vec<u8>, Error, String) {
    let (log, _capture) = support::log::capture(false);
    let mut chunks = match call.await {
        Ok(output) => all(output).await,
        Err(error) => vec![Err(error)],
    };
    let error = chunks
        .pop()
        .expect("an error")
        .expect_err("an error, not an end");

    (joined(chunks), error, log.text())
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
        error.detail().and_then(quoll::Problem::detail),
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
async fn a_stream_with_a_length_is_sent_as_content_length() {
    let (url, api, _) = served().await;

    let response = reqwest::get(format!("{url}/api/sized?count=3&length=24"))
        .await
        .expect("the server answers");
    let headers = response.headers().clone();

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(headers[CONTENT_LENGTH], "24");
    assert!(!headers.contains_key(TRANSFER_ENCODING), "{headers:?}");
    assert_eq!(headers[CONTENT_TYPE], "text/plain");
    assert_eq!(headers["x-content-type-options"], "nosniff");
    assert_eq!(headers[CACHE_CONTROL], "no-cache");
    assert_eq!(headers["x-accel-buffering"], "no");
    assert_eq!(
        response.text().await.expect("the body reads"),
        "chunk 1\nchunk 2\nchunk 3\n"
    );

    let output = api.output().sized(3, 24).await.expect("chunks");

    assert_eq!(output.length(), Some(24));
    assert_eq!(output.content_type(), "text/plain");
    assert_eq!(joined(all(output).await), b"chunk 1\nchunk 2\nchunk 3\n");
}

#[tokio::test]
async fn a_stream_with_no_length_has_none_on_the_client() {
    let (_, api, _) = served().await;

    let output = api.output().chunks(2).await.expect("chunks");

    assert_eq!(output.length(), None);
}

#[tokio::test]
async fn a_length_of_0_sends_an_empty_body_and_never_polls_the_stream() {
    let (log, _capture) = support::log::capture(false);
    let (_, api, mut signals) = served().await;

    let output = api.output().empty_sized().await.expect("chunks");

    assert_eq!(output.length(), Some(0));
    assert!(all(output).await.is_empty());

    // The stream is dropped before it says it was polled, if it ever is.
    assert_eq!(dropped(&mut signals).await, "empty_sized");
    assert!(signals.try_recv().is_err());

    let log = log.text();

    assert!(!log.contains("cut short"), "{log}");
}

#[tokio::test]
async fn a_client_stream_partly_read_is_forwarded_with_the_length_left() {
    let (_, api, _) = served().await;
    let read = Arc::default();
    let front = quoll::test::spawn(Front::router(FrontServices {
        proxy: Forward {
            api,
            read: Arc::clone(&read),
        },
    }))
    .await;
    let front = Front::client(&front).expect("the url is the api's");
    let whole: Vec<u8> = (1..=3).flat_map(chunk).collect();

    let output = front.proxy().forwarded().await.expect("chunks");
    let read = read
        .lock()
        .expect("no test panics holding the lock")
        .expect("the front read a chunk");

    assert_eq!(output.length(), Some((whole.len() - read) as u64));
    assert_eq!(joined(all(output).await), whole[read..]);
}

#[tokio::test]
async fn the_length_counts_down_as_the_chunks_are_read() {
    let (_, api, _) = served().await;

    let mut output = api.output().sized(3, 24).await.expect("chunks");
    let mut read = 0;

    while let Some(chunk) = output.next().await {
        read += chunk.expect("a chunk").len() as u64;

        assert_eq!(output.length(), Some(24 - read));
    }

    assert_eq!(read, 24);
}

#[tokio::test]
async fn a_head_of_a_stream_with_a_length_says_it_with_no_body() {
    let (url, _, _) = served().await;

    let response = reqwest::Client::new()
        .head(format!("{url}/api/sized?count=3&length=24"))
        .send()
        .await
        .expect("the server answers");

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(response.headers()[CONTENT_LENGTH], "24");
    assert!(response.bytes().await.expect("no body").is_empty());
}

#[tokio::test]
async fn a_stream_longer_than_its_length_is_cut_before_its_last_bytes() {
    let (_, api, _) = served().await;

    // The third chunk is one too many: the second, which brought the bytes to the length, is never sent.
    let (before, error, log) = cut_short(api.output().sized(3, 16)).await;

    assert!(b"chunk 1\n".starts_with(&before), "{before:?}");
    assert_eq!(error.status(), None, "{error}");
    assert!(
        log.contains(
            "cut short after its 200: its `ByteStream` gave more than its length, 16 bytes"
        ),
        "{log}"
    );

    // The second chunk goes past the length on its own.
    let (before, _, log) = cut_short(api.output().sized(2, 12)).await;

    assert!(b"chunk 1\n".starts_with(&before), "{before:?}");
    assert!(log.contains("gave more than its length, 12 bytes"), "{log}");
}

#[tokio::test]
async fn a_stream_shorter_than_its_length_cuts_the_connection() {
    let (_, api, _) = served().await;

    let (before, error, log) = cut_short(api.output().sized(2, 24)).await;

    assert!(b"chunk 1\nchunk 2\n".starts_with(&before), "{before:?}");
    assert_eq!(error.status(), None, "{error}");
    assert!(
        log.contains("cut short after its 200: its `ByteStream` ended after 16 of its 24 bytes"),
        "{log}"
    );
}

#[tokio::test]
async fn a_stream_with_a_length_that_fails_cuts_the_connection() {
    let (_, api, _) = served().await;

    let (before, error, log) = cut_short(api.output().sized_failing()).await;

    assert!(b"chunk 1\n".starts_with(&before), "{before:?}");
    assert_eq!(error.status(), None, "{error}");
    assert!(log.contains("the disk went away"), "{log}");
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
        let chunks: Vec<std::result::Result<&'static [u8], std::convert::Infallible>> =
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
                    // A whole body, which axum sends with its `Content-Length`.
                    "count=4" => ([(CONTENT_TYPE, "text/plain")], "whole").into_response(),
                    // "whole" in gzip, with the `Content-Length` of the gzip, which the client decodes.
                    "count=5" => {
                        let gzip: &[u8] = b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\x03\x2b\xcf\xc8\xcf\x49\x05\x00\x4f\x29\x59\xc4\x05\x00\x00\x00";

                        (
                            [(CONTENT_TYPE, "text/plain"), (CONTENT_ENCODING, "gzip")],
                            gzip,
                        )
                            .into_response()
                    }
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
async fn the_content_length_of_another_api_is_the_length() {
    let (api, _, _) = third_party_client().await;

    let whole = api.output().chunks(4).await.expect("a body");

    assert_eq!(whole.length(), Some(5));
    assert_eq!(joined(all(whole).await), b"whole");

    let chunked = api.output().chunks(3).await.expect("chunks");

    assert_eq!(chunked.length(), None);
}

#[tokio::test]
async fn an_answer_the_client_decodes_has_no_length() {
    let (api, _, _) = third_party_client().await;

    let decoded = api.output().chunks(5).await.expect("a body");

    assert_eq!(decoded.length(), None);
    assert_eq!(joined(all(decoded).await), b"whole");
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
