//! The layers of `quoll::server`, each on the router a contract becomes, served on a port and called over HTTP.

#![cfg(all(feature = "server", feature = "client"))]

use std::sync::Arc;
use std::time::Duration;

use futures_util::{StreamExt, stream};
use quoll::rest::{self, Authenticate, Bytes, Events, Form, JsonLines, Result, Text};
use reqwest::StatusCode;
use reqwest::header::{CONTENT_ENCODING, CONTENT_TYPE};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::Notify;

use crate::support::log::Log;

mod support {
    pub(crate) mod log;
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Note {
    id: i64,
    title: String,
}

rest::api! {
    api Api = "/api";

    scheme Session = cookie "session";

    group Layers {
        GET  "/notes/{id}"  => get(id: i64) -> Note;
        OPTIONS "/notes/{id}" => options(id: i64) -> String;
        GET  "/request-id"  => request_id() -> String;
        GET  "/slow"        => slow() -> String;
        GET  "/failing"     => failing() -> String;
        GET  "/unavailable" => unavailable() -> String;
        GET  "/large"       => large() -> Vec<Note>;
        GET  "/held/events" => held_events() -> Events<Note>;
        GET  "/held/lines"  => held_lines() -> JsonLines<Note>;
        GET  "/late/events" => late_events() -> Events<Note>;
        GET  "/late/lines"  => late_lines() -> JsonLines<Note>;
        POST "/json"        => json(note: Note);
        POST "/form"        => form(note: Form<Note>);
        POST "/text"        => text(text: Text);
        POST "/bytes"       => bytes(bytes: Bytes);
    }

    #[auth(Session)]
    group Sessions {
        POST "/session/touch" => touch();
    }
}

/// The deadline of the tests of `timeout`: short, as a route that waits for ever reaches it in each of them.
const DEADLINE: Duration = Duration::from_millis(100);

/// The body limit of the tests of `body_limit`.
const LIMIT: usize = 1024;

/// The service: a held stream sends its first note, then waits for `release` before its second.
#[derive(Clone, Default)]
struct App {
    release: Arc<Notify>,
}

fn note(id: i64) -> Note {
    Note {
        id,
        title: format!("note {id}"),
    }
}

impl App {
    /// Note 1, then note 2 once the test releases it.
    fn held(&self) -> impl futures_util::Stream<Item = Note> + Send + 'static {
        let release = Arc::clone(&self.release);

        stream::once(async { note(1) }).chain(stream::once(async move {
            release.notified().await;
            note(2)
        }))
    }
}

/// Note 1, then note 2 after three times the deadline of `timeout`: a stream that lasts longer than it.
fn late() -> impl futures_util::Stream<Item = Note> + Send + 'static {
    stream::once(async { note(1) }).chain(stream::once(async {
        tokio::time::sleep(DEADLINE * 3).await;
        note(2)
    }))
}

impl LayersServer for App {
    async fn get(&self, id: i64) -> Result<Note> {
        Ok(note(id))
    }

    async fn options(&self, id: i64) -> Result<String> {
        Ok(format!("options {id}"))
    }

    async fn request_id(&self) -> Result<String> {
        Ok(quoll::server::RequestId::current().map_or_else(String::new, |id| id.to_string()))
    }

    async fn slow(&self) -> Result<String> {
        std::future::pending().await
    }

    async fn failing(&self) -> Result<String> {
        Err(rest::Error::internal("the store went away"))
    }

    async fn unavailable(&self) -> Result<String> {
        Err(rest::Error::new(
            StatusCode::SERVICE_UNAVAILABLE,
            "the store is closed",
        ))
    }

    async fn large(&self) -> Result<Vec<Note>> {
        Ok((1..=200).map(note).collect())
    }

    async fn held_events(&self) -> Result<Events<Note>> {
        Ok(Events::new(self.held()))
    }

    async fn held_lines(&self) -> Result<JsonLines<Note>> {
        Ok(JsonLines::new(self.held()))
    }

    async fn late_events(&self) -> Result<Events<Note>> {
        Ok(Events::new(late()))
    }

    async fn late_lines(&self) -> Result<JsonLines<Note>> {
        Ok(JsonLines::new(late()))
    }

    async fn json(&self, _note: Note) -> Result<()> {
        Ok(())
    }

    async fn form(&self, _note: Form<Note>) -> Result<()> {
        Ok(())
    }

    async fn text(&self, _text: Text) -> Result<()> {
        Ok(())
    }

    async fn bytes(&self, _bytes: Bytes) -> Result<()> {
        Ok(())
    }
}

impl SessionsServer for App {
    async fn touch(&self, _caller: &()) -> Result<()> {
        Ok(())
    }
}

impl Authenticate<Session> for App {
    type Caller = ();

    async fn authenticate(&self, _session: String) -> Result<()> {
        Ok(())
    }
}

/// The api's router, before any layer.
fn router(app: &App) -> axum::Router {
    Api::router(ApiServices {
        layers: app.clone(),
        sessions: app.clone(),
    })
}

/// The router as the spec layers it: the last layer added runs first.
fn layered(app: &App) -> axum::Router {
    router(app)
        .layer(quoll::server::body_limit(LIMIT))
        .layer(quoll::server::timeout(DEADLINE))
        .layer(quoll::server::trace())
        .layer(quoll::server::request_id())
}

/// `router` served on a port: its URL, and the client of its api.
async fn serve(router: axum::Router) -> (String, ApiClient) {
    let url = quoll::test::spawn(router).await;
    let api = Api::client(&url).expect("the URL is one");

    (url, api)
}

/// The status, the content type and the detail of a problem.
async fn problem(response: reqwest::Response) -> (StatusCode, String, String) {
    let status = response.status();
    let content_type = response.headers()[CONTENT_TYPE]
        .to_str()
        .expect("a content type is text")
        .to_owned();
    let problem: Value = response.json().await.expect("a problem is JSON");

    assert_eq!(problem["status"], status.as_u16(), "{problem}");

    let detail = problem["detail"].as_str().unwrap_or_default().to_owned();

    (status, content_type, detail)
}

/// Whether `id` is written as a UUID v4.
fn is_uuid_v4(id: &str) -> bool {
    let groups: Vec<usize> = id.split('-').map(str::len).collect();

    groups == [8, 4, 4, 4, 12]
        && id.chars().all(|c| c == '-' || c.is_ascii_hexdigit())
        && id.as_bytes()[14] == b'4'
}

fn request_id_of(response: &reqwest::Response) -> String {
    response.headers()["x-request-id"]
        .to_str()
        .expect("a request id is text")
        .to_owned()
}

#[tokio::test]
async fn a_request_with_no_id_gets_one_which_its_handler_reads() -> Result<()> {
    let (url, api) = serve(layered(&App::default())).await;

    let response = reqwest::get(format!("{url}/api/request-id"))
        .await
        .expect("the server answers");
    let id = request_id_of(&response);
    let read: String = response.json().await.expect("a string");

    assert!(is_uuid_v4(&id), "{id}");
    assert_eq!(read, id);

    let other = api.layers().request_id().await?;

    assert!(is_uuid_v4(&other), "{other}");
    assert_ne!(other, id);
    Ok(())
}

#[tokio::test]
async fn a_caller_id_is_kept_and_sent_back() {
    let (url, _) = serve(layered(&App::default())).await;

    let response = reqwest::Client::new()
        .get(format!("{url}/api/request-id"))
        .header("x-request-id", "req-42_a.b:c")
        .send()
        .await
        .expect("the server answers");
    let id = request_id_of(&response);
    let read: String = response.json().await.expect("a string");

    assert_eq!(id, "req-42_a.b:c");
    assert_eq!(read, "req-42_a.b:c");
}

#[tokio::test]
async fn a_caller_id_that_is_not_kept_is_replaced() {
    let (url, _) = serve(layered(&App::default())).await;
    let long = "a".repeat(129);
    let cases: [(&str, &[u8]); 3] = [
        ("too long", long.as_bytes()),
        ("a space", b"two words"),
        ("not ASCII", "caf\u{e9}".as_bytes()),
    ];

    for (case, given) in cases {
        let value = reqwest::header::HeaderValue::from_bytes(given).expect("a header's value");
        let response = reqwest::Client::new()
            .get(format!("{url}/api/request-id"))
            .header("x-request-id", value)
            .send()
            .await
            .expect("the server answers");
        let id = request_id_of(&response);
        let read: String = response.json().await.expect("a string");

        assert!(is_uuid_v4(&id), "{case}: {id}");
        assert_eq!(read, id, "{case}");
    }
}

#[tokio::test]
async fn a_route_puts_itself_in_its_answer() {
    let router = router(&App::default()).layer(axum::middleware::map_response(
        |mut response: axum::response::Response| async move {
            let operation = response
                .extensions()
                .get::<rest::Route>()
                .map_or("none", rest::Route::operation_id);

            response.headers_mut().insert(
                "x-operation",
                reqwest::header::HeaderValue::from_static(operation),
            );
            response
        },
    ));
    let (url, _) = serve(router).await;

    let found = reqwest::get(format!("{url}/api/notes/3"))
        .await
        .expect("the server answers");
    let failed = reqwest::get(format!("{url}/api/failing"))
        .await
        .expect("the server answers");
    let missing = reqwest::get(format!("{url}/api/nowhere"))
        .await
        .expect("the server answers");

    assert_eq!(found.headers()["x-operation"], "layers_get");
    assert_eq!(failed.headers()["x-operation"], "layers_failing");
    assert_eq!(missing.headers()["x-operation"], "none");
}

/// The lines of the crate's own events in `log`.
fn quoll_lines(log: &Log) -> Vec<String> {
    log.text()
        .lines()
        .filter(|line| line.contains("quoll::"))
        .map(str::to_owned)
        .collect()
}

/// `GET path` with the request id `id`: its status.
async fn get_with_id(url: &str, path: &str, id: &str) -> StatusCode {
    reqwest::Client::new()
        .get(format!("{url}{path}"))
        .header("x-request-id", id)
        .send()
        .await
        .expect("the server answers")
        .status()
}

#[tokio::test]
async fn a_request_is_traced_with_its_operation_and_its_id() {
    let (log, _capture) = support::log::capture(false);
    let (url, _) = serve(layered(&App::default())).await;

    assert_eq!(
        get_with_id(&url, "/api/notes/3?secret=1", "trace-1").await,
        StatusCode::OK
    );

    let lines = quoll_lines(&log);
    let [answered] = lines.as_slice() else {
        panic!("one line per answer: {lines:#?}");
    };

    assert!(answered.contains(" INFO "), "{answered}");
    assert!(answered.contains("request{"), "{answered}");
    assert!(answered.contains("method=GET"), "{answered}");
    assert!(answered.contains("/api/notes/3"), "{answered}");
    assert!(
        !answered.contains("secret"),
        "the query is not logged: {answered}"
    );
    assert!(answered.contains("operation=\"layers_get\""), "{answered}");
    assert!(answered.contains("request_id=trace-1"), "{answered}");
    assert!(answered.contains("answered status=200"), "{answered}");
}

#[tokio::test]
async fn a_request_gets_its_id_in_its_span_whichever_layer_runs_first() {
    let (log, _capture) = support::log::capture(false);
    let router = router(&App::default())
        .layer(quoll::server::request_id())
        .layer(quoll::server::trace());
    let (url, _) = serve(router).await;

    assert_eq!(
        get_with_id(&url, "/api/notes/3", "trace-2").await,
        StatusCode::OK
    );

    let lines = quoll_lines(&log);

    assert_eq!(lines.len(), 1, "{lines:#?}");
    assert!(lines[0].contains("request_id=trace-2"), "{lines:#?}");
}

#[tokio::test]
async fn an_internal_error_is_logged_once_as_an_error() {
    let (log, _capture) = support::log::capture(false);
    let (url, _) = serve(layered(&App::default())).await;

    assert_eq!(
        get_with_id(&url, "/api/failing", "trace-3").await,
        StatusCode::INTERNAL_SERVER_ERROR
    );

    let lines = quoll_lines(&log);
    let [cause, answered] = lines.as_slice() else {
        panic!("the cause, then the answer: {lines:#?}");
    };

    assert!(cause.contains(" ERROR "), "{cause}");
    assert!(cause.contains("the store went away"), "{cause}");
    assert!(cause.contains("operation=\"layers_failing\""), "{cause}");
    assert!(cause.contains("request_id=trace-3"), "{cause}");
    assert!(answered.contains(" INFO "), "{answered}");
    assert!(answered.contains("answered status=500"), "{answered}");
}

#[tokio::test]
async fn a_server_error_a_handler_gives_is_logged_as_an_error() {
    let (log, _capture) = support::log::capture(false);
    let (url, _) = serve(layered(&App::default())).await;

    assert_eq!(
        get_with_id(&url, "/api/unavailable", "trace-4").await,
        StatusCode::SERVICE_UNAVAILABLE
    );

    let lines = quoll_lines(&log);
    let [answered] = lines.as_slice() else {
        panic!("one line per answer: {lines:#?}");
    };

    assert!(answered.contains(" ERROR "), "{answered}");
    assert!(answered.contains("answered status=503"), "{answered}");
}

#[tokio::test]
async fn a_client_error_is_logged_as_information() {
    let (log, _capture) = support::log::capture(false);
    let (url, _) = serve(layered(&App::default())).await;

    assert_eq!(
        get_with_id(&url, "/api/notes/three", "trace-5").await,
        StatusCode::BAD_REQUEST
    );

    let lines = quoll_lines(&log);

    assert_eq!(lines.len(), 1, "{lines:#?}");
    assert!(lines[0].contains(" INFO "), "{lines:#?}");
    assert!(lines[0].contains("answered status=400"), "{lines:#?}");
}

#[tokio::test]
async fn a_route_past_its_deadline_answers_503_logged_once() {
    let (log, _capture) = support::log::capture(false);
    let (url, api) = serve(layered(&App::default())).await;

    let response = reqwest::Client::new()
        .get(format!("{url}/api/slow"))
        .header("x-request-id", "trace-6")
        .send()
        .await
        .expect("the server answers");
    let id = request_id_of(&response);
    let (status, content_type, detail) = problem(response).await;

    assert_eq!(status, StatusCode::SERVICE_UNAVAILABLE);
    assert_eq!(content_type, "application/problem+json");
    assert_eq!(detail, "the server took longer than 100ms to answer");
    assert_eq!(id, "trace-6");

    let lines = quoll_lines(&log);
    let [cause, answered] = lines.as_slice() else {
        panic!("the deadline, then the answer: {lines:#?}");
    };

    assert!(cause.contains(" ERROR "), "{cause}");
    assert!(cause.contains("deadline"), "{cause}");
    assert!(cause.contains("operation=\"layers_slow\""), "{cause}");
    assert!(answered.contains(" INFO "), "{answered}");
    assert!(answered.contains("answered status=503"), "{answered}");

    let error = api.layers().slow().await.expect_err("past the deadline");

    assert_eq!(error.status(), Some(StatusCode::SERVICE_UNAVAILABLE));
    assert_eq!(
        error.problem().and_then(rest::Problem::detail),
        Some("the server took longer than 100ms to answer")
    );
}

#[tokio::test]
async fn a_stream_outlives_the_deadline() -> Result<()> {
    let (_, api) = serve(layered(&App::default())).await;

    let events: Vec<Note> = api
        .layers()
        .late_events()
        .await?
        .map(|note| note.expect("a note"))
        .collect()
        .await;
    let lines: Vec<Note> = api
        .layers()
        .late_lines()
        .await?
        .map(|note| note.expect("a note"))
        .collect()
        .await;

    assert_eq!(events, [note(1), note(2)]);
    assert_eq!(lines, [note(1), note(2)]);
    Ok(())
}

/// A body of `size` bytes sent by the client to each route that reads one, JSON, a form, text and bytes: what each
/// call gives.
async fn post_each(api: &ApiClient, size: usize) -> Vec<(&'static str, Result<()>)> {
    let title = "x".repeat(size);
    let note = Note {
        id: 1,
        title: title.clone(),
    };

    vec![
        ("json", api.layers().json(note.clone()).await),
        ("form", api.layers().form(Form(note)).await),
        ("text", api.layers().text(Text(title)).await),
        (
            "bytes",
            api.layers().bytes(Bytes::new(vec![b'x'; size])).await,
        ),
    ]
}

#[tokio::test]
async fn a_body_with_a_length_past_the_limit_answers_413() {
    let (_, api) = serve(layered(&App::default())).await;

    for (case, answered) in post_each(&api, LIMIT + 1).await {
        let error = answered.expect_err(case);

        assert_eq!(
            error.status(),
            Some(StatusCode::PAYLOAD_TOO_LARGE),
            "{case}"
        );
        assert_eq!(
            error.problem().and_then(rest::Problem::detail),
            Some("the body is larger than the 1024 bytes this server reads"),
            "{case}"
        );
    }

    for (case, answered) in post_each(&api, LIMIT / 2).await {
        answered.unwrap_or_else(|error| panic!("{case}: {error}"));
    }
}

#[tokio::test]
async fn a_large_upload_past_the_limit_reads_its_413() {
    let (_, api) = serve(layered(&App::default())).await;

    // The server answers before reading the body, then closes: the client reads the answer, each time, rather than a
    // connection reset.
    for attempt in 0..10 {
        let error = api
            .layers()
            .bytes(Bytes::new(vec![b'x'; 16 << 20]))
            .await
            .expect_err("past the limit");

        assert_eq!(
            error.status(),
            Some(StatusCode::PAYLOAD_TOO_LARGE),
            "attempt {attempt}: {error:?}"
        );
    }
}

/// `POST path` with a chunked body of `size` bytes of `content_type`, written by hand, as no client here sends one:
/// the status, the content type and the body of the answer. The request is small enough for the server to read it
/// whole from its socket, so that closing the connection with the body unread does not reset it.
async fn post_chunked(
    url: &str,
    path: &str,
    content_type: &str,
    size: usize,
) -> (u16, String, Value) {
    let address = url.trim_start_matches("http://");
    let mut stream = tokio::net::TcpStream::connect(address)
        .await
        .expect("the server listens");
    let head = format!(
        "POST {path} HTTP/1.1\r\nHost: {address}\r\nContent-Type: {content_type}\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
    );
    let mut request = head.into_bytes();
    // In chunks of 256 bytes, so that no one chunk is past the limit alone.
    let body = vec![b'x'; size];

    for chunk in body.chunks(256) {
        request.extend_from_slice(format!("{:x}\r\n", chunk.len()).as_bytes());
        request.extend_from_slice(chunk);
        request.extend_from_slice(b"\r\n");
    }

    request.extend_from_slice(b"0\r\n\r\n");

    stream
        .write_all(&request)
        .await
        .expect("the request is sent");

    let mut answer = Vec::new();

    stream
        .read_to_end(&mut answer)
        .await
        .expect("the answer is read");

    let answer = String::from_utf8(answer).expect("the answer is text");
    let (head, body) = answer.split_once("\r\n\r\n").expect("a head and a body");
    let status = head[9..12].parse().expect("a status");
    let content_type = head
        .lines()
        .find_map(|line| line.strip_prefix("content-type: "))
        .unwrap_or_default()
        .to_owned();
    let body = if body.is_empty() {
        Value::Null
    } else {
        serde_json::from_str(body).expect("a JSON body")
    };

    (status, content_type, body)
}

#[tokio::test]
async fn a_chunked_body_past_the_limit_answers_413() {
    let (url, _) = serve(layered(&App::default())).await;

    for (path, content_type) in [
        ("/api/json", "application/json"),
        ("/api/form", "application/x-www-form-urlencoded"),
        ("/api/text", "text/plain"),
        ("/api/bytes", "application/octet-stream"),
    ] {
        let (status, answered_type, problem) =
            post_chunked(&url, path, content_type, LIMIT * 2).await;

        assert_eq!(status, 413, "{path}");
        assert_eq!(answered_type, "application/problem+json", "{path}");
        assert_eq!(
            problem["detail"], "the body is larger than this server reads",
            "{path}"
        );
    }
}

#[tokio::test]
async fn a_lower_limit_of_a_route_is_not_said_as_the_layer_s() {
    let router = router(&App::default())
        .layer(axum::extract::DefaultBodyLimit::max(100))
        .layer(quoll::server::body_limit(LIMIT));
    let (url, api) = serve(router).await;

    for (case, answered) in post_each(&api, LIMIT / 2).await {
        let error = answered.expect_err(case);

        assert_eq!(
            error.status(),
            Some(StatusCode::PAYLOAD_TOO_LARGE),
            "{case}"
        );
        assert_eq!(
            error.problem().and_then(rest::Problem::detail),
            Some("the body is larger than this server reads"),
            "{case}"
        );
    }

    let (status, _, problem) = post_chunked(&url, "/api/text", "text/plain", LIMIT / 2).await;

    assert_eq!(status, 413);
    assert_eq!(
        problem["detail"],
        "the body is larger than this server reads"
    );
}

const ORIGIN: &str = "https://app.example.com";

fn with_cors(app: &App) -> axum::Router {
    router(app).layer(quoll::server::cors(["HTTPS://App.Example.com:443"]).expect("an origin"))
}

#[tokio::test]
async fn a_preflight_of_a_listed_origin_is_allowed() {
    let (url, _) = serve(with_cors(&App::default())).await;

    let response = reqwest::Client::new()
        .request(reqwest::Method::OPTIONS, format!("{url}/api/notes/3"))
        .header("origin", ORIGIN)
        .header("access-control-request-method", "PUT")
        .header("access-control-request-headers", "x-api-key, content-type")
        .send()
        .await
        .expect("the server answers");
    let headers = response.headers();

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(headers["access-control-allow-origin"], ORIGIN);
    assert_eq!(
        headers["access-control-allow-methods"],
        "GET,HEAD,POST,PUT,PATCH,DELETE,OPTIONS"
    );
    assert_eq!(
        headers["access-control-allow-headers"],
        "x-api-key, content-type"
    );
    assert_eq!(headers["access-control-max-age"], "3600");
    assert!(!headers.contains_key("access-control-allow-credentials"));
}

#[tokio::test]
async fn an_options_call_reaches_its_route_through_cors() -> Result<()> {
    let (url, api) = serve(with_cors(&App::default())).await;

    let response = reqwest::Client::new()
        .request(reqwest::Method::OPTIONS, format!("{url}/api/notes/3"))
        .header("origin", ORIGIN)
        .send()
        .await
        .expect("the server answers");
    let allowed = response.headers()["access-control-allow-origin"].clone();
    let exposed = response.headers()["access-control-expose-headers"].clone();
    let read: String = response.json().await.expect("the route's answer");

    assert_eq!(read, "options 3");
    assert_eq!(allowed, ORIGIN);
    assert_eq!(exposed, "location,www-authenticate,x-request-id");
    assert_eq!(api.layers().options(4).await?, "options 4");
    Ok(())
}

#[tokio::test]
async fn a_call_of_a_listed_origin_reads_its_answer_and_another_does_not() {
    let (url, _) = serve(with_cors(&App::default())).await;
    let http = reqwest::Client::new();

    let listed = http
        .get(format!("{url}/api/notes/3"))
        .header("origin", ORIGIN)
        .send()
        .await
        .expect("the server answers");
    let other = http
        .get(format!("{url}/api/notes/3"))
        .header("origin", "https://evil.example.com")
        .send()
        .await
        .expect("the server answers");

    assert_eq!(listed.headers()["access-control-allow-origin"], ORIGIN);
    assert_eq!(
        listed.headers()["access-control-expose-headers"],
        "location,www-authenticate,x-request-id"
    );
    assert!(!other.headers().contains_key("access-control-allow-origin"));
}

#[tokio::test]
async fn a_listed_origin_is_still_refused_by_a_cookie_scheme() {
    let (url, _) = serve(with_cors(&App::default())).await;

    let response = reqwest::Client::new()
        .post(format!("{url}/api/session/touch"))
        .header("origin", ORIGIN)
        .header("cookie", "session=s-ana")
        .send()
        .await
        .expect("the server answers");

    assert_eq!(response.headers()["access-control-allow-origin"], ORIGIN);
    assert_eq!(problem(response).await.0, StatusCode::FORBIDDEN);
}

#[test]
fn cors_refuses_what_is_not_an_origin() {
    let error = quoll::server::cors([ORIGIN, "*"]).expect_err("`*` is no origin");

    assert_eq!(error.origin(), "*");
}

/// A client that reads an answer as it comes, and sends the `Accept-Encoding` it is given.
fn raw_client() -> reqwest::Client {
    reqwest::Client::builder()
        .no_gzip()
        .build()
        .expect("a client")
}

#[tokio::test]
async fn an_answer_is_compressed_as_the_caller_accepts() -> Result<()> {
    let router = router(&App::default()).layer(quoll::server::compression());
    let (url, api) = serve(router).await;

    for encoding in ["gzip", "br", "zstd"] {
        let response = raw_client()
            .get(format!("{url}/api/large"))
            .header("accept-encoding", encoding)
            .send()
            .await
            .expect("the server answers");

        assert_eq!(response.headers()[CONTENT_ENCODING], encoding);
    }

    let plain = raw_client()
        .get(format!("{url}/api/large"))
        .send()
        .await
        .expect("the server answers");

    assert!(!plain.headers().contains_key(CONTENT_ENCODING));

    // The client asks for gzip, and reads it back.
    assert_eq!(api.layers().large().await?.len(), 200);
    Ok(())
}

#[tokio::test]
async fn a_stream_is_not_compressed_and_its_first_value_comes_at_once() -> Result<()> {
    // Read raw from a service of its own, whose held streams no test releases.
    let (url, _) = serve(router(&App::default()).layer(quoll::server::compression())).await;

    for path in ["/api/held/events", "/api/held/lines"] {
        let response = raw_client()
            .get(format!("{url}{path}"))
            .header("accept-encoding", "gzip, br, zstd")
            .send()
            .await
            .expect("the server answers");

        assert!(!response.headers().contains_key(CONTENT_ENCODING), "{path}");
    }

    let app = App::default();
    let (_, api) = serve(router(&app).layer(quoll::server::compression())).await;
    // A first value held in an encoder would never come: the guard fails the test rather than hang it.
    let guard = Duration::from_secs(10);

    let mut events = api.layers().held_events().await?;
    let first = tokio::time::timeout(guard, events.next())
        .await
        .expect("the first event comes");

    assert_eq!(first.expect("an event")?, note(1));
    app.release.notify_one();
    assert_eq!(events.next().await.expect("an event")?, note(2));

    let mut lines = api.layers().held_lines().await?;
    let first = tokio::time::timeout(guard, lines.next())
        .await
        .expect("the first line comes");

    assert_eq!(first.expect("a line")?, note(1));
    app.release.notify_one();
    assert_eq!(lines.next().await.expect("a line")?, note(2));
    Ok(())
}
