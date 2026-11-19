//! The results that are not a plain JSON value: `Accepted<T>`, `Text` and `Bytes`, answered by the server a contract
//! becomes and read back by its client, served on a port.

#![cfg(all(feature = "server", feature = "client"))]

use std::error::Error as _;

use quoll::rest::{self, Accepted, Bytes, Error, Result, Text};
use reqwest::StatusCode;
use reqwest::header::CONTENT_TYPE;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
struct Export {
    name: String,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
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
        GET  "/missing"            => missing() -> rest::Text;
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
        Ok(Bytes(pattern(len)))
    }

    async fn missing(&self) -> Result<Text> {
        Err(Error::not_found("no such text"))
    }
}

/// `len` bytes going through every value of a byte, so that none is lost or changed on the way.
fn pattern(len: usize) -> Vec<u8> {
    (0..len).map(|i| (i % 251) as u8).collect()
}

/// The api served on a port, its URL and its client.
async fn served() -> (String, ApiClient) {
    let url = rest::test::spawn(Api::router(ApiServices { files: App })).await;
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
        assert!(bytes.0 == pattern(len), "{len}");
    }
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
    use axum::response::IntoResponse;
    use axum::routing::get;

    let answer = |content_type: Option<&'static str>, body: &'static [u8]| {
        let mut response = body.into_response();
        let headers = response.headers_mut();

        headers.remove(CONTENT_TYPE);
        if let Some(content_type) = content_type {
            headers.insert(CONTENT_TYPE, HeaderValue::from_static(content_type));
        }

        response
    };

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
    let url = rest::test::spawn(third_party()).await;
    let api = Api::client(&url).expect("the url is the api's");

    let plain = api.files().text("plain".into()).await.expect("text/plain");
    assert_eq!(plain, Text::from("no charset"));

    let quoted = api.files().text("quoted".into()).await.expect("UTF-8");
    assert_eq!(quoted, Text::from("quoted"));
}

#[tokio::test]
async fn text_of_another_content_type_or_charset_is_an_error_with_no_status() {
    let url = rest::test::spawn(third_party()).await;
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
    let url = rest::test::spawn(third_party()).await;
    let api = Api::client(&url).expect("the url is the api's");

    let error = api
        .files()
        .text("not-utf8".into())
        .await
        .expect_err("not UTF-8");

    not_its_result(&error, "files_text", "invalid utf-8");
}

#[tokio::test]
async fn bytes_are_read_from_an_octet_stream_only() {
    let url = rest::test::spawn(third_party()).await;
    let api = Api::client(&url).expect("the url is the api's");

    let bytes = api.files().bytes(0).await.expect("an octet stream");
    assert_eq!(bytes, Bytes::from(vec![0x00, 0xff]));

    for len in [1, 2, 3] {
        let error = api.files().bytes(len).await.expect_err("not bytes");

        not_its_result(
            &error,
            "files_bytes",
            "its content type is not `application/octet-stream`",
        );
    }
}
