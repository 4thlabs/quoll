//! The kind of each error of the client, as the real client gives it from a server of raw TCP on a free port that
//! refuses, holds, closes, answers another content type or JSON that is not the result, sends a line that does not
//! read, or cuts a body; through a middleware, `retry` and `trace`; and as a fake builds it.

#![cfg(feature = "client")]

use std::fmt;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{Stream, StreamExt, stream};
use http::{Extensions, StatusCode};
use quoll::client::{self, Attempt, Retry};
use quoll::{ByteStream, Call, Error, ErrorKind, Events, JsonLines, Problem, Text};
use reqwest_middleware::{Middleware, Next};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpSocket, TcpStream};

mod support {
    pub(crate) mod log;
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Note {
    id: i64,
}

/// What another API answers on an error, as GitHub's.
#[derive(Debug, Deserialize, JsonSchema)]
struct GitHubError {
    message: String,
}

impl fmt::Display for GitHubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

quoll::api! {
    api Api = "/api";

    group Notes {
        GET "/notes/{id}" => get(id: i64) -> Note;
        GET "/text"       => text() -> Text;
        GET "/lines"      => lines() -> JsonLines<Note>;
        GET "/events"     => events() -> Events<Note>;
        GET "/bytes"      => bytes() -> ByteStream;
    }
}

quoll::api! {
    api Secured = "/api";

    #[auth(Bearer)]
    group Secure {
        GET "/me" => me() -> String;
    }
}

quoll::api! {
    #[error(GitHubError)]
    api GitHub = "";

    group Repos {
        GET "/repos/{owner}" => get(owner: &str) -> String;
    }
}

/// What the raw server does once it has read the head of a request.
#[derive(Clone)]
enum Then {
    /// Writes these bytes, then closes the connection.
    Answer(Arc<[u8]>),
    /// Closes the connection with no answer.
    Close,
    /// Keeps the connection open and answers nothing.
    Hold,
    /// Resets the connection with no answer.
    Reset,
}

/// A server of raw TCP on a free port that does `then` on each connection: its URL, and the count of its connections.
async fn raw(then: Then) -> (String, Arc<AtomicUsize>) {
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("a free port");
    let url = format!("http://{}", listener.local_addr().expect("an address"));
    let count = Arc::new(AtomicUsize::new(0));
    let counted = Arc::clone(&count);

    tokio::spawn(async move {
        let mut held = Vec::new();

        while let Ok((connection, _)) = listener.accept().await {
            counted.fetch_add(1, Ordering::SeqCst);

            if let Some(connection) = serve(connection, then.clone()).await {
                held.push(connection);
            }
        }
    });

    (url, count)
}

/// Reads the head of one request on `connection`, then does `then`: the connection, when it is held.
async fn serve(mut connection: TcpStream, then: Then) -> Option<TcpStream> {
    let mut head = Vec::new();
    let mut byte = [0; 1];

    while !head.ends_with(b"\r\n\r\n") {
        match connection.read(&mut byte).await {
            Ok(1) => head.push(byte[0]),
            _ => return None,
        }
    }

    match then {
        Then::Answer(bytes) => {
            // The client may have gone: what it would read is not the test's.
            let _ = connection.write_all(&bytes).await;
            let _ = connection.shutdown().await;

            None
        }
        Then::Close => None,
        Then::Hold => Some(connection),
        Then::Reset => {
            // With no linger, closing sends a reset rather than an end.
            connection.set_zero_linger().expect("no linger");

            None
        }
    }
}

/// An answer of status 200 with `content_type` and `body`, of its exact length.
fn ok(content_type: &str, body: &[u8]) -> Then {
    let head = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close",
        body.len()
    );

    cut(&head, body)
}

/// An answer whose head is `head`, a status line and its headers without the blank line, and whose body is `body` as
/// it is, cut wherever it ends.
fn cut(head: &str, body: &[u8]) -> Then {
    let mut answer = format!("{head}\r\n\r\n").into_bytes();

    answer.extend_from_slice(body);
    Then::Answer(answer.into())
}

/// A URL where nothing listens: a port kept bound and not listening, which refuses each connection. The socket goes
/// with the URL, so that no other test takes the port while it is kept.
fn refused() -> (String, TcpSocket) {
    let socket = TcpSocket::new_v4().expect("a socket");

    socket
        .bind("127.0.0.1:0".parse().expect("an address"))
        .expect("a bound socket");

    let url = format!("http://{}", socket.local_addr().expect("an address"));

    (url, socket)
}

/// The client of the api at `url`, with no middleware.
fn api(url: &str) -> ApiClient {
    Api::client(url).expect("the url is the api's")
}

/// The kind of `error`, after checking that its status is `Some(s)` exactly when its kind is `Status(s)`.
fn kind<E: fmt::Debug>(error: &Error<E>) -> ErrorKind {
    let kind = error.kind();
    let status = match kind {
        ErrorKind::Status(status) => Some(status),
        _ => None,
    };

    assert_eq!(error.status(), status, "{error:?}");
    kind
}

/// Every item of `stream`, each a value or the kind of its error, to its end.
async fn items<T>(stream: impl Stream<Item = quoll::Result<T>>) -> Vec<Result<T, ErrorKind>> {
    stream
        .map(|item| item.map_err(|error| kind(&error)))
        .collect()
        .await
}

#[tokio::test]
async fn a_refused_connection_is_connect() {
    let (url, _socket) = refused();

    let error = api(&url).notes().get(1).await.expect_err("refused");

    assert_eq!(kind(&error), ErrorKind::Connect);
    assert!(!kind(&error).answered());
    assert_eq!(error.to_string(), "the call `notes_get` got no response");
}

#[tokio::test]
async fn a_name_that_does_not_resolve_is_connect() {
    let error = api("http://no-such-host.invalid")
        .notes()
        .get(1)
        .await
        .expect_err("no such host");

    assert_eq!(kind(&error), ErrorKind::Connect);
}

#[tokio::test]
async fn a_deadline_passed_is_timeout() {
    let (url, _) = raw(Then::Hold).await;

    let error = api(&url)
        .notes()
        .get(1)
        .timeout(Duration::from_millis(200))
        .await
        .expect_err("past its deadline");

    assert_eq!(kind(&error), ErrorKind::Timeout);
}

#[tokio::test]
async fn the_head_of_a_stream_past_its_deadline_is_timeout_with_or_without_the_builder() {
    let (url, _) = raw(Then::Hold).await;

    // With no middleware, the call bounds the head itself.
    let alone = api(&url)
        .notes()
        .lines()
        .timeout(Duration::from_millis(200))
        .await
        .expect_err("past its deadline");
    // Through the builder, its `client::timeout` bounds it.
    let built = client::builder()
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .lines()
        .timeout(Duration::from_millis(200))
        .await
        .expect_err("past its deadline");

    assert_eq!(kind(&alone), ErrorKind::Timeout);
    assert_eq!(kind(&built), ErrorKind::Timeout);
}

#[tokio::test]
async fn a_connection_closed_before_the_answer_is_closed() {
    let (url, _) = raw(Then::Close).await;

    let error = api(&url).notes().get(1).await.expect_err("no answer");

    assert_eq!(kind(&error), ErrorKind::Closed);
}

#[tokio::test]
async fn a_connection_reset_before_the_answer_is_closed_and_tried_again() {
    let (url, resets) = raw(Then::Reset).await;

    let alone = api(&url).notes().get(1).await.expect_err("reset");
    let retried = client::builder()
        .with(client::retry(2))
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("reset");

    assert_eq!(kind(&alone), ErrorKind::Closed);
    assert_eq!(kind(&retried), ErrorKind::Closed);
    assert_eq!(resets.load(Ordering::SeqCst), 4, "once, then three times");
}

#[tokio::test]
async fn an_answer_of_another_content_type_is_decode() {
    let (url, _) = raw(ok("text/html", b"<html>")).await;

    let error = api(&url).notes().get(1).await.expect_err("not JSON");

    assert_eq!(kind(&error), ErrorKind::Decode);
    assert!(kind(&error).answered());
}

#[tokio::test]
async fn json_that_is_not_the_result_is_decode() {
    let (url, _) = raw(ok("application/json", br#"{"id":"one"}"#)).await;

    let error = api(&url).notes().get(1).await.expect_err("not a note");

    assert_eq!(kind(&error), ErrorKind::Decode);
    assert_eq!(
        error.to_string(),
        "the call `notes_get` got an answer that is not its result"
    );
}

#[tokio::test]
async fn a_text_that_is_not_utf8_is_decode() {
    let (url, _) = raw(ok("text/plain; charset=utf-8", b"caf\xe9")).await;

    let error = api(&url).notes().text().await.expect_err("not UTF-8");

    assert_eq!(kind(&error), ErrorKind::Decode);
}

#[tokio::test]
async fn a_stream_of_another_content_type_is_decode_at_the_call() {
    let (url, _) = raw(ok("text/html", b"<html>")).await;

    let error = api(&url).notes().lines().await.expect_err("not JSON Lines");

    assert_eq!(kind(&error), ErrorKind::Decode);
}

#[tokio::test]
async fn a_whole_body_cut_short_is_body() {
    let (url, _) = raw(cut(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100",
        br#"{"id""#,
    ))
    .await;

    let error = api(&url).notes().get(1).await.expect_err("cut");

    assert_eq!(kind(&error), ErrorKind::Body);
}

#[tokio::test]
async fn an_error_status_is_its_kind_with_its_detail_or_without_one() {
    let (problem, _) = raw(cut(
        "HTTP/1.1 404 Not Found\r\nContent-Type: application/problem+json\r\nContent-Length: 20",
        br#"{"detail":"no note"}"#,
    ))
    .await;
    // A proxy's page, which is not the api's error shape.
    let (proxy, _) = raw(cut(
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 18",
        b"404 page not found",
    ))
    .await;

    let problem = api(&problem).notes().get(1).await.expect_err("a 404");
    let proxy = api(&proxy).notes().get(1).await.expect_err("a 404");

    assert_eq!(kind(&problem), ErrorKind::Status(StatusCode::NOT_FOUND));
    assert_eq!(problem.detail().and_then(Problem::detail), Some("no note"));
    assert_eq!(kind(&proxy), ErrorKind::Status(StatusCode::NOT_FOUND));
    assert!(proxy.detail().is_none());
    assert_eq!(proxy.body(), Some(&b"404 page not found"[..]));
}

#[tokio::test]
async fn a_line_that_does_not_read_is_decode_and_the_lines_go_on() {
    let body = b"{\"id\":1}\nnot json\n{\"id\":2}\n";
    let (url, _) = raw(ok("application/jsonl", body)).await;

    let lines = api(&url).notes().lines().await.expect("a stream");

    assert_eq!(
        items(lines).await,
        [
            Ok(Note { id: 1 }),
            Err(ErrorKind::Decode),
            Ok(Note { id: 2 })
        ]
    );
}

#[tokio::test]
async fn lines_cut_mid_stream_are_body_and_the_stream_ends() {
    // A chunk of one line, then the connection closes with no last chunk.
    let (url, _) = raw(cut(
        "HTTP/1.1 200 OK\r\nContent-Type: application/jsonl\r\nTransfer-Encoding: chunked",
        b"9\r\n{\"id\":1}\n\r\n",
    ))
    .await;

    let mut lines = api(&url).notes().lines().await.expect("a stream");
    let first = lines.next().await.map(Result::ok);
    let cut = lines.next().await.expect("an item").expect_err("cut");

    assert_eq!(first, Some(Some(Note { id: 1 })));
    assert_eq!(kind(&cut), ErrorKind::Body);
    assert!(lines.next().await.is_none(), "the stream ends after it");
}

#[tokio::test]
async fn a_line_past_the_limit_is_body_and_the_stream_ends() {
    let mut body = vec![b'1'; 1024 * 1024 + 1];

    body.extend_from_slice(b"\n{\"id\":2}\n");

    let (url, _) = raw(ok("application/jsonl", &body)).await;

    let lines = api(&url).notes().lines().await.expect("a stream");

    assert_eq!(items(lines).await, [Err(ErrorKind::Body)]);
}

#[tokio::test]
async fn an_event_that_does_not_read_is_decode_and_the_events_go_on() {
    let body =
        b"data: {\"id\":1}\n\ndata: nope\n\nevent: problem\ndata: [1]\n\ndata: {\"id\":2}\n\n";
    let (url, _) = raw(ok("text/event-stream", body)).await;

    let events = api(&url).notes().events().await.expect("a stream");

    // An `event: problem` whose data is not a problem is an event that does not read: the stream goes on.
    assert_eq!(
        items(events).await,
        [
            Ok(Note { id: 1 }),
            Err(ErrorKind::Decode),
            Err(ErrorKind::Decode),
            Ok(Note { id: 2 })
        ]
    );
}

#[tokio::test]
async fn a_problem_that_ends_the_events_is_status() {
    let body = b"data: {\"id\":1}\n\nevent: problem\ndata: {\"status\":503,\"detail\":\"down\"}\n\ndata: {\"id\":2}\n\n";
    let (url, _) = raw(ok("text/event-stream", body)).await;

    let mut events = api(&url).notes().events().await.expect("a stream");
    let first = events.next().await.map(Result::ok);
    let problem = events
        .next()
        .await
        .expect("an item")
        .expect_err("the problem");

    assert_eq!(first, Some(Some(Note { id: 1 })));
    assert_eq!(
        kind(&problem),
        ErrorKind::Status(StatusCode::SERVICE_UNAVAILABLE)
    );
    assert_eq!(problem.detail().and_then(Problem::detail), Some("down"));
    assert!(events.next().await.is_none(), "the stream ends after it");
}

#[tokio::test]
async fn bytes_cut_short_of_their_length_are_body_and_the_stream_ends() {
    let (url, _) = raw(cut(
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 10",
        b"abcd",
    ))
    .await;

    let bytes = api(&url).notes().bytes().await.expect("a stream");

    assert_eq!(
        items(bytes).await,
        [Ok(b"abcd".to_vec()), Err(ErrorKind::Body)]
    );
}

#[tokio::test]
async fn a_url_that_is_not_one_is_invalid() {
    let error = Api::client("ftp://localhost").expect_err("not http");

    assert_eq!(kind(&error), ErrorKind::Invalid);
}

#[tokio::test]
async fn a_redirect_loop_is_middleware() {
    let (url, _) = raw(cut(
        "HTTP/1.1 302 Found\r\nLocation: /api/notes/1\r\nContent-Length: 0\r\nConnection: close",
        b"",
    ))
    .await;

    let error = client::builder()
        .with(client::redirects(5))
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("a loop");

    assert_eq!(kind(&error), ErrorKind::Middleware);
}

#[tokio::test]
async fn a_head_that_is_not_http_is_decode_and_is_not_tried_again() {
    // What a server of another protocol writes first, as SSH does, or a TLS port called over `http`.
    let (url, answers) = raw(Then::Answer(Arc::from(&b"SSH-2.0-OpenSSH_9.6\r\n"[..]))).await;

    let error = client::builder()
        .with(client::retry(2))
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("not HTTP");

    assert_eq!(kind(&error), ErrorKind::Decode);
    assert_eq!(
        answers.load(Ordering::SeqCst),
        1,
        "it fails again: not tried again"
    );
}

/// A source of tokens that asks each of an auth server at its URL, through a quoll client of its own.
struct Remote {
    url: String,
}

impl client::TokenSource for Remote {
    async fn token(&self) -> quoll::Result<String> {
        let token = api(&self.url).notes().text().await?;

        Ok(token.0)
    }

    async fn refresh(&self, _refused: &str) -> quoll::Result<String> {
        self.token().await
    }
}

#[tokio::test]
async fn a_token_that_could_not_be_had_is_middleware_and_keeps_its_own_kind_in_its_sources() {
    // The api answers; the auth server refuses the connection.
    let (url, calls) = raw(ok("application/json", br#""ana""#)).await;
    let (auth, _socket) = refused();

    let error = client::builder()
        .with(client::bearer_from(Remote { url: auth }))
        .build::<Secured>(&url)
        .expect("a client")
        .secure()
        .me()
        .await
        .expect_err("no token");

    assert_eq!(kind(&error), ErrorKind::Middleware);
    assert_eq!(calls.load(Ordering::SeqCst), 0, "the api was not called");

    let mut source = std::error::Error::source(&error);
    let mut token = None;

    while let Some(error) = source {
        if let Some(reqwest_middleware::Error::Middleware(error)) =
            error.downcast_ref::<reqwest_middleware::Error>()
        {
            token = error.downcast_ref::<Error>().map(Error::kind);
        }

        source = error.source();
    }

    assert_eq!(token, Some(ErrorKind::Connect), "{error:?}");
}

#[tokio::test]
async fn a_middleware_whose_own_call_to_an_api_of_another_shape_failed_is_middleware() {
    let (url, calls) = raw(ok("application/json", br#"{"id":1}"#)).await;
    let (other, _socket) = refused();
    let looked_up = middleware(move |request, extensions, next| {
        let other = other.clone();

        Box::pin(async move {
            // A call of the middleware's own, whose error is a `quoll::Error<GitHubError>`.
            GitHub::client(&other)
                .expect("a client")
                .repos()
                .get("ana")
                .await
                .map_err(reqwest_middleware::Error::middleware)?;

            next.run(request, extensions).await
        })
    });

    let error = client::builder()
        .with(looked_up)
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("the middleware fails");

    assert_eq!(kind(&error), ErrorKind::Middleware);
    assert_eq!(calls.load(Ordering::SeqCst), 0, "the api was not called");
}

/// What a middleware's call returns.
type Handled<'a> = std::pin::Pin<
    Box<dyn Future<Output = reqwest_middleware::Result<reqwest::Response>> + Send + 'a>,
>;

/// Gives a closure the signature reqwest-middleware takes, for every lifetime of its request.
fn middleware<F>(handle: F) -> F
where
    F: for<'a> Fn(reqwest::Request, &'a mut Extensions, Next<'a>) -> Handled<'a>
        + Send
        + Sync
        + 'static,
{
    handle
}

/// An error type of the application's that keeps the chain's error as its source, as reqwest-retry's `RetryError`.
#[derive(Debug)]
struct GaveUp(reqwest_middleware::Error);

impl fmt::Display for GaveUp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("gave up")
    }
}

impl std::error::Error for GaveUp {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.0)
    }
}

/// A middleware that wraps each failure of the rest of the chain in a `GaveUp`.
fn wrapping() -> impl Middleware {
    middleware(|request, extensions, next| {
        Box::pin(async move {
            next.run(request, extensions)
                .await
                .map_err(|error| reqwest_middleware::Error::middleware(GaveUp(error)))
        })
    })
}

#[tokio::test]
async fn an_error_of_a_middleware_of_its_own_is_middleware() {
    let (url, _) = raw(Then::Close).await;
    let sealed = middleware(|_, _, _| {
        Box::pin(async {
            Err(reqwest_middleware::Error::middleware(
                std::io::Error::other("the vault is sealed"),
            ))
        })
    });

    let error = client::builder()
        .with(sealed)
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("sealed");

    assert_eq!(kind(&error), ErrorKind::Middleware);
}

/// Whether a reqwest error among the sources of `error` says its connection could not be opened.
fn caused_by_connect(error: &(dyn std::error::Error + 'static)) -> bool {
    std::iter::successors(error.source(), |&error| error.source()).any(|source| {
        let reqwest = match source.downcast_ref::<reqwest_middleware::Error>() {
            Some(reqwest_middleware::Error::Reqwest(error)) => Some(error),
            _ => source.downcast_ref::<reqwest::Error>(),
        };

        reqwest.is_some_and(reqwest::Error::is_connect)
    })
}

#[tokio::test]
async fn an_error_of_the_chain_a_middleware_wraps_is_middleware_and_keeps_its_cause() {
    let (url, _socket) = refused();

    let error = client::builder()
        .with(wrapping())
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("refused");

    assert_eq!(kind(&error), ErrorKind::Middleware);
    assert!(caused_by_connect(&error), "{error:?}");
}

#[tokio::test]
async fn a_reqwest_error_a_middleware_passes_through_as_it_is_keeps_its_kind() {
    let (url, _socket) = refused();
    let passing = middleware(|request, extensions, next| {
        Box::pin(async move {
            next.run(request, extensions)
                .await
                .map_err(|error| match error {
                    reqwest_middleware::Error::Reqwest(error) => {
                        reqwest_middleware::Error::middleware(error)
                    }
                    error => error,
                })
        })
    });

    let error = client::builder()
        .with(passing)
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("refused");

    assert_eq!(kind(&error), ErrorKind::Connect);
}

/// An error of the application's that is the `quoll::Error` it holds, its message and its sources, as thiserror's
/// `#[error(transparent)]` writes it.
#[derive(Debug)]
struct Transparent(Error<GitHubError>);

impl fmt::Display for Transparent {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.0, f)
    }
}

impl std::error::Error for Transparent {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        std::error::Error::source(&self.0)
    }
}

#[tokio::test]
async fn a_middleware_that_fails_with_a_transparent_wrapper_of_a_quoll_error_is_middleware() {
    let (url, calls) = raw(ok("application/json", br#"{"id":1}"#)).await;
    let (other, _socket) = refused();
    let looked_up = middleware(move |request, extensions, next| {
        let other = other.clone();

        Box::pin(async move {
            GitHub::client(&other)
                .expect("a client")
                .repos()
                .get("ana")
                .await
                .map_err(|error| reqwest_middleware::Error::middleware(Transparent(error)))?;

            next.run(request, extensions).await
        })
    });

    let error = client::builder()
        .with(looked_up)
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("the middleware fails");

    assert_eq!(kind(&error), ErrorKind::Middleware);
    assert!(caused_by_connect(&error), "{error:?}");
    assert_eq!(calls.load(Ordering::SeqCst), 0, "the api was not called");
}

#[tokio::test]
async fn retry_and_kind_agree_on_an_error_of_the_chain_a_middleware_wraps() {
    let (url, resets) = raw(Then::Reset).await;
    let seen = Arc::new(Mutex::new(Vec::new()));
    let kinds = Arc::clone(&seen);
    let retry = client::retry(2).when(move |attempt: &Attempt| {
        kinds.lock().expect("no panic").push(attempt.kind());

        attempt.transient()
    });

    // `retry` first, then the wrapper: each attempt's error is the wrapper's.
    let error = client::builder()
        .with(retry)
        .with(wrapping())
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("reset");

    assert_eq!(kind(&error), ErrorKind::Middleware);
    assert_eq!(
        *seen.lock().expect("no panic"),
        [Some(ErrorKind::Middleware)]
    );
    assert_eq!(
        resets.load(Ordering::SeqCst),
        1,
        "a middleware's error is not tried again"
    );
}

#[tokio::test]
async fn retry_tries_again_after_a_connection_closed_and_not_after_an_answer_that_does_not_read() {
    let (closed, closes) = raw(Then::Close).await;
    let (undecoded, answers) = raw(ok("text/html", b"<html>")).await;
    let call = |url: String| async move {
        client::builder()
            .with(client::retry(2))
            .build::<Api>(&url)
            .expect("a client")
            .notes()
            .get(1)
            .await
            .expect_err("fails")
    };

    assert_eq!(kind(&call(closed).await), ErrorKind::Closed);
    assert_eq!(kind(&call(undecoded).await), ErrorKind::Decode);
    assert_eq!(closes.load(Ordering::SeqCst), 3, "tried twice more");
    assert_eq!(
        answers.load(Ordering::SeqCst),
        1,
        "an answer is not tried again"
    );
}

#[tokio::test]
async fn a_rule_of_retry_reads_the_kind_of_an_attempt() {
    let (url, closes) = raw(Then::Close).await;
    let seen = Arc::new(Mutex::new(Vec::new()));
    let kinds = Arc::clone(&seen);
    let retry = client::retry(3).when(move |attempt: &Attempt| {
        kinds.lock().expect("no panic").push(attempt.kind());

        match attempt.kind() {
            Some(ErrorKind::Connect) => Retry::Backoff,
            _ => Retry::No,
        }
    });

    let error = client::builder()
        .with(retry)
        .build::<Api>(&url)
        .expect("a client")
        .notes()
        .get(1)
        .await
        .expect_err("closed");

    assert_eq!(kind(&error), ErrorKind::Closed);
    assert_eq!(*seen.lock().expect("no panic"), [Some(ErrorKind::Closed)]);
    assert_eq!(
        closes.load(Ordering::SeqCst),
        1,
        "a closed connection is not refused"
    );
}

#[tokio::test]
async fn a_rule_of_retry_reads_an_error_status_as_a_kind_and_a_success_as_none() {
    let (unavailable, answers) = raw(cut(
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close",
        b"",
    ))
    .await;
    let (fine, _) = raw(ok("application/json", br#"{"id":1}"#)).await;
    let seen = Arc::new(Mutex::new(Vec::new()));
    let kinds = Arc::clone(&seen);
    let retry = client::retry(2).when(move |attempt: &Attempt| {
        kinds.lock().expect("no panic").push(attempt.kind());

        match attempt.kind() {
            Some(ErrorKind::Status(StatusCode::SERVICE_UNAVAILABLE)) => Retry::Backoff,
            _ => Retry::No,
        }
    });
    let api = |url: &str| {
        client::builder()
            .with(retry.clone())
            .build::<Api>(url)
            .expect("a client")
    };

    let error = api(&unavailable).notes().get(1).await.expect_err("a 503");
    let note = api(&fine).notes().get(1).await.expect("a note");

    assert_eq!(
        kind(&error),
        ErrorKind::Status(StatusCode::SERVICE_UNAVAILABLE)
    );
    assert_eq!(answers.load(Ordering::SeqCst), 3, "tried twice more");
    assert_eq!(note, Note { id: 1 });
    assert_eq!(
        *seen.lock().expect("no panic"),
        [
            Some(ErrorKind::Status(StatusCode::SERVICE_UNAVAILABLE)),
            Some(ErrorKind::Status(StatusCode::SERVICE_UNAVAILABLE)),
            Some(ErrorKind::Status(StatusCode::SERVICE_UNAVAILABLE)),
            None,
        ]
    );
}

#[tokio::test]
async fn trace_records_the_kind_of_a_failure_and_the_code_of_an_error_status_as_error_type() {
    let (log, _capture) = support::log::capture(true);
    let (refused, _socket) = refused();
    let (not_found, _) = raw(cut(
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close",
        b"",
    ))
    .await;
    let (fine, _) = raw(ok("application/json", br#"{"id":1}"#)).await;
    let traced = |url: &str| {
        client::builder()
            .with(client::trace())
            .build::<Api>(url)
            .expect("a client")
    };

    traced(&refused).notes().get(1).await.expect_err("refused");
    traced(&not_found).notes().get(1).await.expect_err("a 404");
    traced(&fine).notes().get(1).await.expect("a note");

    let log = log.text();
    let spans: Vec<&str> = log
        .lines()
        .filter(|line| line.contains("HTTP request"))
        .collect();

    assert_eq!(spans.len(), 3, "{log}");
    assert!(spans[0].contains("error.type=\"connect\""), "{}", spans[0]);
    assert!(spans[1].contains("error.type=\"404\""), "{}", spans[1]);
    assert!(!spans[2].contains("error.type"), "{}", spans[2]);
}

/// A fake whose `get` fails with `kind`, and whose lines give a note, an item that does not read, a note, then a cut.
struct Fake {
    kind: ErrorKind,
}

impl Notes for Fake {
    fn get(&self, _id: i64) -> impl Call<Note> {
        let kind = self.kind;

        async move { Err(Error::failed(kind, "the fake fails")) }
    }

    fn text(&self) -> impl Call<Text> {
        async { Ok(Text::from("ok")) }
    }

    fn lines(&self) -> impl Call<JsonLines<Note>> {
        async {
            Ok(JsonLines::try_new(stream::iter([
                Ok(Note { id: 1 }),
                Err(Error::failed(ErrorKind::Decode, "not a note")),
                Ok(Note { id: 2 }),
                Err(Error::failed(ErrorKind::Body, "cut")),
            ])))
        }
    }

    fn events(&self) -> impl Call<Events<Note>> {
        async { Ok(Events::new(stream::empty())) }
    }

    fn bytes(&self) -> impl Call<ByteStream> {
        async { Ok(ByteStream::new(stream::empty())) }
    }
}

#[tokio::test]
async fn a_fake_fails_with_the_kind_it_is_given() {
    let fake = Fake {
        kind: ErrorKind::Timeout,
    };

    let error = fake.get(1).await.expect_err("fails");
    let lines = items(fake.lines().await.expect("a stream")).await;

    assert_eq!(kind(&error), ErrorKind::Timeout);
    assert_eq!(
        lines,
        [
            Ok(Note { id: 1 }),
            Err(ErrorKind::Decode),
            Ok(Note { id: 2 }),
            Err(ErrorKind::Body)
        ]
    );
}

#[tokio::test]
async fn the_response_of_a_fake_is_what_the_real_client_gives_for_its_kind() {
    let connect = Fake {
        kind: ErrorKind::Connect,
    };
    // A proxy's 404, whose body is not the api's error shape: a status and no detail.
    let proxied = Fake {
        kind: ErrorKind::Status(StatusCode::NOT_FOUND),
    };

    let refused = connect.get(1).response().await.expect_err("no answer");
    let error = proxied.get(1).await.expect_err("a 404");
    let answer = proxied.get(1).response().await.expect("an answer of 404");

    assert_eq!(kind(&refused), ErrorKind::Connect);
    assert_eq!(kind(&error), ErrorKind::Status(StatusCode::NOT_FOUND));
    assert!(error.detail().is_none());
    assert!(error.body().is_none(), "nothing came");
    assert_eq!(
        error.to_string(),
        "another api answered 404: the fake fails"
    );
    assert_eq!(answer.status(), StatusCode::NOT_FOUND);
}

/// A fake of an api of another shape.
struct FakeRepos;

impl Repos for FakeRepos {
    fn get(&self, owner: &str) -> impl Call<String, GitHubError> {
        let slow = owner == "slow";

        async move {
            if slow {
                // `E` is the one of the call: `failed` needs no `into_shape`.
                return Err(Error::failed(ErrorKind::Timeout, "the fake is slow"));
            }

            Err(Error::answered(
                StatusCode::NOT_FOUND,
                GitHubError {
                    message: "Not Found".into(),
                },
            ))
        }
    }
}

#[tokio::test]
async fn a_fake_of_another_shape_fails_with_a_kind_with_no_into_shape() {
    let slow = FakeRepos.get("slow").await.expect_err("slow");
    let missing = FakeRepos.get("ana").await.expect_err("missing");

    assert_eq!(kind(&slow), ErrorKind::Timeout);
    assert_eq!(kind(&missing), ErrorKind::Status(StatusCode::NOT_FOUND));
    assert_eq!(
        missing.detail().map(|detail| detail.message.as_str()),
        Some("Not Found")
    );
}
