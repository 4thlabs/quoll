//! The results that are not a plain JSON value: `Accepted<T>`, `Text` and `Bytes`, answered by the server a contract
//! becomes and read back by its client, served on a port.

#![cfg(all(feature = "server", feature = "client"))]

use std::error::Error as _;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use quoll::rest::{self, Accepted, Bytes, Error, Result, Text};
use reqwest::StatusCode;
use reqwest::header::CONTENT_TYPE;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct Export {
    name: String,
}

#[derive(Debug, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Job {
    id: u64,
    name: String,
}

rest::api! {
    api Api = "/api";

    group Files {
        POST "/exports"           => export(export: Export) -> Accepted<Job>;
        GET  "/text?value={value}" => text(value: String) -> Text;
        GET  "/bytes?len={len}"    => bytes(len: usize) -> Bytes;
        GET  "/image?type={content_type}" => image(content_type: Option<String>) -> Bytes;
        GET  "/missing"            => missing() -> rest::Text;
    }

    /// Routes of each kind of result, to see the `Accept` each sends.
    group Accepts {
        GET    "/accept"    => json() -> String;
        DELETE "/accept"    => none();
        GET    "/preferred" => preferred(header("Accept") accept: Option<String>) -> Text;
    }
}

struct App;

impl FilesServer for App {
    async fn export(&self, export: Export) -> Result<Accepted<Job>> {
        Ok(Accepted::new(Job {
            id: 7,
            name: export.name,
        }))
    }

    async fn text(&self, value: String) -> Result<Text> {
        Ok(Text(value))
    }

    async fn bytes(&self, len: usize) -> Result<Bytes> {
        Ok(Bytes::new(pattern(len)))
    }

    async fn image(&self, content_type: Option<String>) -> Result<Bytes> {
        let png = Bytes::new(PNG);

        Ok(match content_type {
            Some(content_type) => png.with_content_type(content_type),
            None => png,
        })
    }

    async fn missing(&self) -> Result<Text> {
        Err(Error::not_found("no such text"))
    }
}

impl AcceptsServer for App {
    async fn json(&self) -> Result<String> {
        Ok(String::new())
    }

    async fn none(&self) -> Result<()> {
        Ok(())
    }

    async fn preferred(&self, accept: Option<String>) -> Result<Text> {
        Ok(Text(accept.unwrap_or_default()))
    }
}

/// The first bytes of a PNG.
const PNG: [u8; 8] = *b"\x89PNG\r\n\x1a\n";

/// `len` bytes going through every value of a byte, so that none is lost or changed on the way.
fn pattern(len: usize) -> Vec<u8> {
    (0..len).map(|i| (i % 251) as u8).collect()
}

/// The api served on a port, its URL and its client.
async fn served() -> (String, ApiClient) {
    let url = quoll::test::spawn(Api::router(ApiServices {
        files: App,
        accepts: App,
    }))
    .await;
    let api = Api::client(&url).expect("the url is the api's");

    (url, api)
}

/// The status, the content type and the body of a request sent as it is.
async fn raw(request: reqwest::RequestBuilder) -> (StatusCode, String, Vec<u8>) {
    let response = request.send().await.expect("the server answers");
    let status = response.status();
    let content_type = response.headers()[CONTENT_TYPE]
        .to_str()
        .expect("a content type is text")
        .to_owned();
    let body = response.bytes().await.expect("the body reads").to_vec();

    (status, content_type, body)
}

#[tokio::test]
async fn accepted_answers_202_with_json_and_the_client_gets_the_value() {
    let (url, api) = served().await;
    let http = reqwest::Client::new();

    let (status, content_type, body) = raw(http
        .post(format!("{url}/api/exports"))
        .header(CONTENT_TYPE, "application/json")
        .body(r#"{"name":"notes.zip"}"#))
    .await;
    assert_eq!(status, StatusCode::ACCEPTED);
    assert_eq!(content_type, "application/json");
    assert_eq!(body, br#"{"id":7,"name":"notes.zip"}"#);

    let job = api
        .files()
        .export(Export {
            name: "notes.zip".into(),
        })
        .await
        .expect("accepted");
    assert_eq!(
        job,
        Job {
            id: 7,
            name: "notes.zip".into()
        }
    );
}

#[tokio::test]
async fn text_answers_200_as_utf8_text_and_reads_back() {
    let (url, api) = served().await;

    let (status, content_type, body) =
        raw(reqwest::Client::new().get(format!("{url}/api/text?value=caf%C3%A9"))).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(content_type, "text/plain; charset=utf-8");
    assert_eq!(body, "café".as_bytes());

    for value in ["milk, eggs", "café ☕ 東京", "line\nbreak", ""] {
        let text = api.files().text(value.into()).await.expect("text");

        assert_eq!(text, Text::from(value), "{value:?}");
    }
}

#[tokio::test]
async fn bytes_answer_200_as_an_octet_stream_and_read_back_whole() {
    let (url, api) = served().await;

    let (status, content_type, body) =
        raw(reqwest::Client::new().get(format!("{url}/api/bytes?len=300"))).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(content_type, "application/octet-stream");
    assert_eq!(body, pattern(300));

    // Empty, every value of a byte, and far more than one read gives, or the 64 KiB an error keeps.
    for len in [0, 256, 3 * 1024 * 1024] {
        let bytes = api.files().bytes(len).await.expect("bytes");

        assert_eq!(bytes.len(), len);
        assert!(bytes.as_ref() == pattern(len), "{len}");
        assert_eq!(bytes.content_type(), "application/octet-stream");
    }
}

#[tokio::test]
async fn bytes_answer_their_content_type_and_read_back_with_it() {
    let (url, api) = served().await;

    let (status, content_type, body) =
        raw(reqwest::Client::new().get(format!("{url}/api/image?type=image%2Fpng"))).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(content_type, "image/png");
    assert_eq!(body, PNG);

    // A browser reads them as their type says, and never runs them as a page.
    for query in ["type=text%2Fhtml", "type=image%2Fsvg%2Bxml", ""] {
        let response = reqwest::get(format!("{url}/api/image?{query}"))
            .await
            .expect("the server answers");

        assert_eq!(
            response.headers()["x-content-type-options"],
            "nosniff",
            "{query}"
        );
    }

    for content_type in ["image/png", "text/csv; charset=utf-8", "Image/SVG+XML"] {
        let image = api
            .files()
            .image(Some(content_type.into()))
            .await
            .expect("bytes");

        assert_eq!(image.content_type(), content_type);
        assert_eq!(image.as_ref(), PNG);
    }

    let image = api.files().image(None).await.expect("bytes");
    assert_eq!(image.content_type(), "application/octet-stream");
    assert_eq!(image, Bytes::new(PNG));
}

#[tokio::test]
async fn bytes_of_a_content_type_that_is_not_a_media_type_answer_500() {
    let (url, api) = served().await;

    for content_type in ["png", "image/png%0D%0Ax-y:%20z", "image/png;%20name=a%20b"] {
        let response = reqwest::get(format!("{url}/api/image?type={content_type}"))
            .await
            .expect("the server answers");
        let status = response.status();
        let problem: serde_json::Value = response.json().await.expect("the body is JSON");

        // Logged, with no detail sent.
        assert_eq!(status, StatusCode::INTERNAL_SERVER_ERROR, "{content_type}");
        assert_eq!(problem.get("detail"), None);
    }

    let error = api
        .files()
        .image(Some("png".into()))
        .await
        .expect_err("a 500");
    assert_eq!(error.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
}

#[tokio::test]
async fn an_error_of_a_text_route_is_a_problem_with_its_status() {
    let (_, api) = served().await;

    let error = api.files().missing().await.expect_err("not found");

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(
        error.problem().and_then(rest::Problem::detail),
        Some("no such text")
    );
}

/// Another API, written without the contract: it answers the text and bytes routes with the body and the content type
/// the query names, `?value=html` or `?len=0`.
fn third_party() -> axum::Router {
    use axum::http::{HeaderValue, Uri};
    use axum::response::{IntoResponse, Response};
    use axum::routing::get;

    fn answer(content_type: Option<&'static str>, body: impl IntoResponse) -> Response {
        let mut response = body.into_response();
        let headers = response.headers_mut();

        headers.remove(CONTENT_TYPE);
        if let Some(content_type) = content_type {
            headers.insert(CONTENT_TYPE, HeaderValue::from_static(content_type));
        }

        response
    }

    axum::Router::new()
        .route(
            "/api/text",
            get(move |uri: Uri| async move {
                match uri.query().unwrap_or_default() {
                    "value=plain" => answer(Some("text/plain"), b"no charset"),
                    "value=quoted" => answer(Some("Text/Plain; Charset=\"UTF-8\""), b"quoted"),
                    "value=html" => answer(Some("text/html; charset=utf-8"), b"<p>a page</p>"),
                    "value=json" => answer(Some("application/json"), b"\"json\""),
                    "value=latin1" => answer(Some("text/plain; charset=iso-8859-1"), b"caf\xe9"),
                    "value=none" => answer(None, b"no content type"),
                    // More than a first read of the connection gives, less than the client reads to drop it.
                    "value=page" => answer(Some("text/html"), "<p>".repeat(16 * 1024)),
                    _ => answer(Some("text/plain; charset=utf-8"), b"caf\xff!"),
                }
            }),
        )
        .route(
            "/api/bytes",
            get(move |uri: Uri| async move {
                match uri.query().unwrap_or_default() {
                    "len=1" => answer(Some("application/json"), b"[1]"),
                    "len=2" => answer(Some("image/png"), b"\x89PNG"),
                    "len=3" => answer(None, b"\x00\x01\x02"),
                    "len=4" => answer(Some("png"), b"\x89PNG"),
                    _ => answer(Some("Application/Octet-Stream; x=y"), b"\x00\xff"),
                }
            }),
        )
}

/// Asserts that `error` is that of an answer that is not the result of `operation`, with no status, and that its
/// source says `why`.
fn not_its_result(error: &Error, operation: &str, why: &str) {
    assert_eq!(error.status(), None, "{error}");
    assert!(error.body().is_none(), "{error}");
    assert_eq!(
        error.to_string(),
        format!("the call `{operation}` got an answer that is not its result")
    );

    let source = error.source().expect("the error says why").to_string();
    assert!(source.contains(why), "{source}");
}

#[tokio::test]
async fn text_is_read_from_text_plain_in_utf8_with_or_without_a_charset() {
    let url = quoll::test::spawn(third_party()).await;
    let api = Api::client(&url).expect("the url is the api's");

    let plain = api.files().text("plain".into()).await.expect("text/plain");
    assert_eq!(plain, Text::from("no charset"));

    let quoted = api.files().text("quoted".into()).await.expect("UTF-8");
    assert_eq!(quoted, Text::from("quoted"));
}

#[tokio::test]
async fn text_of_another_content_type_or_charset_is_an_error_with_no_status() {
    let url = quoll::test::spawn(third_party()).await;
    let api = Api::client(&url).expect("the url is the api's");

    for value in ["html", "json", "latin1", "none"] {
        let error = api.files().text(value.into()).await.expect_err(value);

        not_its_result(
            &error,
            "files_text",
            "its content type is not `text/plain` in UTF-8",
        );
    }
}

#[tokio::test]
async fn text_that_is_not_utf8_is_an_error_with_no_status() {
    let url = quoll::test::spawn(third_party()).await;
    let api = Api::client(&url).expect("the url is the api's");

    let error = api
        .files()
        .text("not-utf8".into())
        .await
        .expect_err("not UTF-8");

    not_its_result(&error, "files_text", "invalid utf-8");

    // The `Utf8Error` alone, not a `FromUtf8Error`, which would keep the whole body.
    let source = error.source().expect("the error says why");
    assert!(source.is::<std::str::Utf8Error>(), "{source:?}");
}

#[tokio::test]
async fn bytes_are_read_of_any_content_type_which_they_keep() {
    let url = quoll::test::spawn(third_party()).await;
    let api = Api::client(&url).expect("the url is the api's");

    for (len, content_type, body) in [
        (0, "Application/Octet-Stream; x=y", &b"\x00\xff"[..]),
        (1, "application/json", b"[1]"),
        (2, "image/png", b"\x89PNG"),
        // No content type, or one that is not a media type: bytes of no known type.
        (3, "application/octet-stream", b"\x00\x01\x02"),
        (4, "application/octet-stream", b"\x89PNG"),
    ] {
        let bytes = api.files().bytes(len).await.expect("bytes");

        assert_eq!(bytes.content_type(), content_type, "{len}");
        assert_eq!(bytes.as_ref(), body, "{len}");
    }
}

#[tokio::test]
async fn an_answer_of_another_content_type_is_read_so_that_its_connection_serves_again() {
    use axum::serve::ListenerExt;

    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port is free");
    let url = format!(
        "http://{}",
        listener.local_addr().expect("the listener has an address")
    );
    let connections = Arc::new(AtomicUsize::new(0));
    let counted = Arc::clone(&connections);
    let listener = listener.tap_io(move |_| {
        counted.fetch_add(1, Ordering::SeqCst);
    });
    tokio::spawn(async move { axum::serve(listener, third_party()).await });
    let api = Api::client(&url).expect("the url is the api's");

    for _ in 0..3 {
        let error = api.files().text("page".into()).await.expect_err("not text");

        not_its_result(
            &error,
            "files_text",
            "its content type is not `text/plain` in UTF-8",
        );
    }

    assert_eq!(connections.load(Ordering::SeqCst), 1);
}

/// The `Accept` header of each request, by its method and its path.
type SeenAccepts = Arc<Mutex<Vec<(String, Option<String>)>>>;

/// A server that keeps the `Accept` header of each request, and answers it as the route's result.
fn accept_recorder(accepts: SeenAccepts) -> axum::Router {
    use axum::http::{HeaderMap, HeaderValue, Method, Uri, header::ACCEPT};
    use axum::response::IntoResponse;

    axum::Router::new().fallback(move |method: Method, uri: Uri, headers: HeaderMap| {
        let accepts = Arc::clone(&accepts);

        async move {
            let values: Vec<&str> = headers
                .get_all(ACCEPT)
                .iter()
                .map(|value| value.to_str().expect("an Accept is text"))
                .collect();
            let accept = (!values.is_empty()).then(|| values.join(", "));
            accepts
                .lock()
                .expect("no test panics holding the lock")
                .push((format!("{method} {}", uri.path()), accept));

            let (status, content_type, body): (_, _, &'static [u8]) = match (method, uri.path()) {
                (Method::POST, "/api/exports") => (
                    StatusCode::ACCEPTED,
                    "application/json",
                    br#"{"id":1,"name":"a"}"#,
                ),
                (_, "/api/text" | "/api/preferred") => (StatusCode::OK, "text/plain", b"text"),
                (_, "/api/bytes") => (StatusCode::OK, "application/octet-stream", b"\x00"),
                (Method::GET, "/api/accept") => (StatusCode::OK, "application/json", b"\"\""),
                _ => (StatusCode::NO_CONTENT, "application/json", b""),
            };

            (
                status,
                [(CONTENT_TYPE, HeaderValue::from_static(content_type))],
                body,
            )
                .into_response()
        }
    })
}

#[tokio::test]
async fn each_call_asks_for_its_result_and_a_route_s_own_accept_wins() {
    let accepts = SeenAccepts::default();
    let url = quoll::test::spawn(accept_recorder(Arc::clone(&accepts))).await;
    let api = Api::client(&url).expect("the url is the api's");

    api.files()
        .export(Export { name: "a".into() })
        .await
        .expect("accepted");
    api.files().text("a".into()).await.expect("text");
    api.files().bytes(1).await.expect("bytes");
    api.accepts().json().await.expect("JSON");
    api.accepts().none().await.expect("nothing");
    api.accepts().preferred(None).await.expect("text");
    api.accepts()
        .preferred(Some("text/*".into()))
        .await
        .expect("text");

    let accepts = accepts.lock().expect("no test panics holding the lock");
    let seen: Vec<(&str, Option<&str>)> = accepts
        .iter()
        .map(|(route, accept)| (route.as_str(), accept.as_deref()))
        .collect();

    assert_eq!(
        seen[..4],
        [
            ("POST /api/exports", Some("application/json")),
            ("GET /api/text", Some("text/plain")),
            ("GET /api/bytes", Some("*/*")),
            ("GET /api/accept", Some("application/json")),
        ]
    );
    // A route with no result asks for nothing of its own: what reqwest sends, if anything.
    assert_eq!(seen[4].0, "DELETE /api/accept");
    assert!(matches!(seen[4].1, None | Some("*/*")), "{seen:?}");
    assert_eq!(
        seen[5..],
        [
            ("GET /api/preferred", Some("text/plain")),
            ("GET /api/preferred", Some("text/*")),
        ]
    );
}
