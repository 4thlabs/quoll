//! The arguments that are not a path parameter or JSON: a query struct, headers, and bodies as a form, text or bytes,
//! sent by the client the contract becomes to the server it becomes, served on a port.

#![cfg(all(feature = "server", feature = "client"))]

use quoll::rest::{self, Bytes, Form, Result, Text};
use reqwest::StatusCode;
use reqwest::header::{CONTENT_TYPE, HeaderValue};
use serde::{Deserialize, Serialize};
use serde_json::Value;

/// A filter whose keys may all be missing: `Default` is for callers, which fill the keys they need.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
struct NoteFilter {
    contains: Option<String>,
    page: Option<u32>,
    #[serde(default, rename = "tag")]
    tags: Vec<String>,
}

/// A query struct without `Default`, whose key is needed.
#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Paging {
    page: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum Shelf {
    KitchenTop,
    Office,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Seen {
    page: u32,
    last: Option<String>,
    shelf: Option<Shelf>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
struct Login {
    user: String,
    password: String,
    #[serde(default, rename = "scope")]
    scopes: Vec<String>,
}

/// A struct, which is not read from one piece of text.
#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Point {
    x: i32,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Mixed {
    id: i64,
    tag: String,
    filter: NoteFilter,
    text: String,
}

rest::api! {
    api Api = "/api";

    group Search {
        GET "/notes"  => search(query filter: NoteFilter) -> NoteFilter;
        GET "/paging" => paging(query paging: Paging) -> Paging;
    }

    group Headers {
        GET "/headers" => read(
            header("X-Page") page: u32,
            header("Last-Event-ID") last: Option<String>,
            header("X-Shelf") shelf: Option<Shelf>,
        ) -> Seen;
    }

    /// A path parameter of a struct type, which the macro cannot tell from a unit enum.
    group Points {
        GET "/points/{point}" => point(point: Point) -> String;
    }

    group Bodies {
        POST "/token" => token(login: Form<Login>) -> Login;
        PUT  "/text"  => text(text: Text) -> String;
        PUT  "/bytes" => bytes(file: Bytes) -> Vec<u8>;
        POST "/mixed/{id}"
            => mixed(text: Text, query filter: NoteFilter, header("X-Tag") tag: String, id: i64) -> Mixed;
    }
}

struct App;

impl SearchServer for App {
    async fn search(&self, filter: NoteFilter) -> Result<NoteFilter> {
        Ok(filter)
    }

    async fn paging(&self, paging: Paging) -> Result<Paging> {
        Ok(paging)
    }
}

impl HeadersServer for App {
    async fn read(&self, page: u32, last: Option<String>, shelf: Option<Shelf>) -> Result<Seen> {
        Ok(Seen { page, last, shelf })
    }
}

impl PointsServer for App {
    async fn point(&self, point: Point) -> Result<String> {
        unreachable!("a struct is never read from the path: {point:?}")
    }
}

impl BodiesServer for App {
    async fn token(&self, login: Form<Login>) -> Result<Login> {
        // A `Form<T>` derefs to its `T`.
        assert!(!login.user.is_empty());

        Ok(login.0)
    }

    async fn text(&self, text: Text) -> Result<String> {
        Ok(text.0)
    }

    async fn bytes(&self, file: Bytes) -> Result<Vec<u8>> {
        Ok(file.0)
    }

    async fn mixed(&self, text: Text, filter: NoteFilter, tag: String, id: i64) -> Result<Mixed> {
        Ok(Mixed {
            id,
            tag,
            filter,
            text: text.to_string(),
        })
    }
}

/// The api served on a port, its URL and its client.
async fn served() -> (String, ApiClient) {
    let router = Api::router(ApiServices {
        search: App,
        headers: App,
        points: App,
        bodies: App,
    });
    let url = rest::test::spawn(router).await;
    let api = Api::client(&url).expect("the url is the api's");

    (url, api)
}

/// The status and the problem a request sent as it is gets.
async fn problem(request: reqwest::RequestBuilder) -> (StatusCode, String) {
    let response = request.send().await.expect("the server answers");
    let status = response.status();
    let content_type = response.headers()[CONTENT_TYPE].clone();
    let problem: Value = response.json().await.expect("the body is JSON");

    assert_eq!(content_type, "application/problem+json");
    assert_eq!(problem["status"], status.as_u16());

    (
        status,
        problem["detail"].as_str().unwrap_or_default().to_owned(),
    )
}

#[tokio::test]
async fn a_query_struct_round_trips_with_its_keys_missing_or_repeated() {
    let (_, api) = served().await;
    let full = NoteFilter {
        contains: Some("milk & eggs?".into()),
        page: Some(2),
        tags: vec!["a".into(), "b c".into(), "é".into()],
    };

    assert_eq!(api.search().search(full.clone()).await.unwrap(), full);
    assert_eq!(
        api.search().search(NoteFilter::default()).await.unwrap(),
        NoteFilter::default()
    );

    let some = NoteFilter {
        page: Some(1),
        ..NoteFilter::default()
    };
    assert_eq!(api.search().search(some.clone()).await.unwrap(), some);
}

#[tokio::test]
async fn a_query_struct_reads_what_a_browser_sends() {
    let (url, _) = served().await;
    let read = |query: &'static str| {
        let url = format!("{url}/api/notes{query}");

        async move {
            reqwest::get(url)
                .await
                .expect("the server answers")
                .json::<NoteFilter>()
                .await
                .expect("the filter")
        }
    };

    assert_eq!(read("").await, NoteFilter::default());
    assert_eq!(
        read("?tag=a&tag=b&page=3").await,
        NoteFilter {
            contains: None,
            page: Some(3),
            tags: vec!["a".into(), "b".into()],
        }
    );
    // An empty value is `Some("")` for a string and `None` for a number, as for a query in the string.
    assert_eq!(
        read("?contains=&page=").await,
        NoteFilter {
            contains: Some(String::new()),
            ..NoteFilter::default()
        }
    );
}

#[tokio::test]
async fn a_query_struct_that_does_not_read_is_a_bad_request() {
    let (url, api) = served().await;

    assert_eq!(
        api.search().paging(Paging { page: 4 }).await.unwrap(),
        Paging { page: 4 }
    );

    let missing = problem(reqwest::Client::new().get(format!("{url}/api/paging"))).await;
    assert_eq!(missing.0, StatusCode::BAD_REQUEST);
    assert!(missing.1.contains("the query is not valid"), "{missing:?}");

    let wrong = problem(reqwest::Client::new().get(format!("{url}/api/paging?page=four"))).await;
    assert_eq!(wrong.0, StatusCode::BAD_REQUEST);

    let not_utf8 =
        problem(reqwest::Client::new().get(format!("{url}/api/notes?contains=%FF"))).await;
    assert_eq!(
        not_utf8,
        (
            StatusCode::BAD_REQUEST,
            "the query is not UTF-8 once decoded".into()
        )
    );
}

#[tokio::test]
async fn headers_round_trip_and_an_optional_one_may_be_missing() {
    let (_, api) = served().await;

    let all = api
        .headers()
        .read(2, Some("event 41".into()), Some(Shelf::KitchenTop))
        .await
        .unwrap();
    assert_eq!(
        all,
        Seen {
            page: 2,
            last: Some("event 41".into()),
            shelf: Some(Shelf::KitchenTop),
        }
    );

    let none = api.headers().read(0, None, None).await.unwrap();
    assert_eq!(
        none,
        Seen {
            page: 0,
            last: None,
            shelf: None,
        }
    );

    // An empty value is a value: `Some("")`, as a browser sends it.
    let empty = api
        .headers()
        .read(1, Some(String::new()), None)
        .await
        .unwrap();
    assert_eq!(empty.last.as_deref(), Some(""));
}

#[tokio::test]
async fn a_header_missing_malformed_or_given_twice_is_a_bad_request() {
    let (url, _) = served().await;
    let http = reqwest::Client::new();
    let get = || http.get(format!("{url}/api/headers"));

    let missing = problem(get()).await;
    assert_eq!(
        missing,
        (
            StatusCode::BAD_REQUEST,
            "the request has no header `X-Page`, which this route takes".into()
        )
    );

    let malformed = problem(get().header("X-Page", "two")).await;
    assert_eq!(
        malformed,
        (
            StatusCode::BAD_REQUEST,
            "the header `X-Page` is not valid: expected an integer that is not negative".into()
        )
    );

    let negative = problem(get().header("X-Page", "-1")).await;
    assert_eq!(negative.0, StatusCode::BAD_REQUEST);

    let unknown = problem(get().header("X-Page", "1").header("X-Shelf", "attic")).await;
    assert_eq!(unknown.0, StatusCode::BAD_REQUEST);
    assert!(
        unknown.1.starts_with("the header `X-Shelf` is not valid"),
        "{unknown:?}"
    );

    let twice = problem(get().header("X-Page", "1").header("x-page", "2")).await;
    assert_eq!(
        twice,
        (
            StatusCode::BAD_REQUEST,
            "the header `X-Page` is given more than once, and this route reads one value".into()
        )
    );

    let latin1 = HeaderValue::from_bytes(b"caf\xE9").expect("a header's value");
    let not_utf8 = problem(get().header("X-Page", "1").header("Last-Event-ID", latin1)).await;
    assert_eq!(
        not_utf8,
        (
            StatusCode::BAD_REQUEST,
            "the header `Last-Event-ID` is not UTF-8".into()
        )
    );

    // A name is read whatever its case.
    let read = get()
        .header("x-PAGE", "5")
        .send()
        .await
        .expect("the server answers")
        .json::<Seen>()
        .await
        .expect("what the server saw");
    assert_eq!(read.page, 5);
}

#[tokio::test]
async fn a_header_that_would_not_read_back_is_refused_by_the_client() {
    let (_, api) = served().await;
    let filter = NoteFilter::default();

    for tag in ["line\nbreak", "nul\0", " leading", "trailing\t"] {
        let error = api
            .bodies()
            .mixed(Text::from("x"), filter.clone(), tag.into(), 1)
            .await
            .expect_err("not a header's value as it is");

        assert_eq!(error.status(), None, "{tag:?}");
        assert!(
            error.to_string().contains("`x-tag` of `bodies_mixed`"),
            "{error}"
        );
        assert!(!error.to_string().contains(tag.trim()), "{error}");
    }
}

#[tokio::test]
async fn a_form_round_trips() {
    let (_, api) = served().await;
    let login = Login {
        user: "ana bé".into(),
        password: "p&ss=wörd+".into(),
        scopes: vec!["notes".into(), "admin".into()],
    };

    assert_eq!(
        api.bodies().token(Form(login.clone())).await.unwrap(),
        login
    );
}

#[tokio::test]
async fn a_form_is_read_only_as_a_form() {
    let (url, _) = served().await;
    let http = reqwest::Client::new();
    let post = |content_type: &'static str, body: &'static [u8]| {
        http.post(format!("{url}/api/token"))
            .header(CONTENT_TYPE, content_type)
            .body(body)
    };

    let read = post(
        "Application/X-WWW-Form-Urlencoded; charset=utf-8",
        b"user=ana&password=x&scope=a&scope=b",
    )
    .send()
    .await
    .expect("the server answers")
    .json::<Login>()
    .await
    .expect("the login");
    assert_eq!(read.scopes, ["a", "b"]);

    let json = problem(post(
        "application/json",
        br#"{"user":"ana","password":"x"}"#,
    ))
    .await;
    assert_eq!(json.0, StatusCode::UNSUPPORTED_MEDIA_TYPE);
    assert!(
        json.1
            .contains("Content-Type: application/x-www-form-urlencoded"),
        "{json:?}"
    );

    let none = problem(
        http.post(format!("{url}/api/token"))
            .body("user=ana&password=x"),
    )
    .await;
    assert_eq!(none.0, StatusCode::UNSUPPORTED_MEDIA_TYPE);

    let latin1 = problem(post(
        "application/x-www-form-urlencoded; charset=iso-8859-1",
        b"user=ana&password=x",
    ))
    .await;
    assert_eq!(latin1.0, StatusCode::UNSUPPORTED_MEDIA_TYPE);

    let missing = problem(post("application/x-www-form-urlencoded", b"user=ana")).await;
    assert_eq!(missing.0, StatusCode::BAD_REQUEST);
    assert!(
        missing.1.starts_with("the form is not valid"),
        "{missing:?}"
    );

    let not_utf8 = problem(post(
        "application/x-www-form-urlencoded",
        b"user=%FF&password=x",
    ))
    .await;
    assert_eq!(
        not_utf8,
        (
            StatusCode::BAD_REQUEST,
            "the form is not UTF-8 once decoded".into()
        )
    );
}

#[tokio::test]
async fn text_round_trips() {
    let (_, api) = served().await;

    for text in ["milk, eggs", "", "é ✓ 日本\r\n\0"] {
        assert_eq!(api.bodies().text(Text::from(text)).await.unwrap(), text);
    }
}

#[tokio::test]
async fn text_is_read_only_as_utf8_text() {
    let (url, _) = served().await;
    let http = reqwest::Client::new();
    let put = |content_type: &'static str, body: &'static [u8]| {
        http.put(format!("{url}/api/text"))
            .header(CONTENT_TYPE, content_type)
            .body(body)
    };
    let read = |content_type: &'static str| async move {
        put(content_type, "milk".as_bytes())
            .send()
            .await
            .expect("the server answers")
            .json::<String>()
            .await
            .expect("the text")
    };

    assert_eq!(read("text/plain").await, "milk");
    assert_eq!(read("Text/Plain; Charset=\"UTF-8\"").await, "milk");
    assert_eq!(read("text/plain; format=flowed").await, "milk");

    let not_utf8 = problem(put("text/plain", b"caf\xE9")).await;
    assert_eq!(
        not_utf8,
        (StatusCode::BAD_REQUEST, "the body is not UTF-8 text".into())
    );

    for content_type in [
        "text/plain; charset=iso-8859-1",
        "application/json",
        "text/html",
    ] {
        let refused = problem(put(content_type, b"milk")).await;

        assert_eq!(
            refused.0,
            StatusCode::UNSUPPORTED_MEDIA_TYPE,
            "{content_type}"
        );
        assert!(
            refused
                .1
                .contains("Content-Type: text/plain; charset=utf-8"),
            "{refused:?}"
        );
    }

    let none = problem(http.put(format!("{url}/api/text")).body("milk")).await;
    assert_eq!(none.0, StatusCode::UNSUPPORTED_MEDIA_TYPE);
}

#[tokio::test]
async fn bytes_round_trip() {
    let (_, api) = served().await;
    let every: Vec<u8> = (0..=255).collect();

    assert_eq!(
        api.bodies().bytes(Bytes(every.clone())).await.unwrap(),
        every
    );
    assert_eq!(
        api.bodies().bytes(Bytes::default()).await.unwrap(),
        Vec::<u8>::new()
    );
}

#[tokio::test]
async fn bytes_are_read_only_as_an_octet_stream() {
    let (url, _) = served().await;
    let http = reqwest::Client::new();
    let put = || http.put(format!("{url}/api/bytes"));

    // A form, or text, that another site's page posts without asking never reaches the route.
    for content_type in [
        "text/plain",
        "application/x-www-form-urlencoded",
        "multipart/form-data",
    ] {
        let refused = problem(put().header(CONTENT_TYPE, content_type).body("x")).await;

        assert_eq!(
            refused.0,
            StatusCode::UNSUPPORTED_MEDIA_TYPE,
            "{content_type}"
        );
    }

    let none = problem(put().body("x")).await;
    assert_eq!(none.0, StatusCode::UNSUPPORTED_MEDIA_TYPE);
}

#[tokio::test]
async fn every_kind_of_argument_goes_together() {
    let (_, api) = served().await;
    let filter = NoteFilter {
        contains: Some("milk".into()),
        page: None,
        tags: vec!["x".into()],
    };

    let mixed = api
        .bodies()
        .mixed(Text::from("a note"), filter.clone(), "tag ✓".into(), 9)
        .await
        .unwrap();

    assert_eq!(
        mixed,
        Mixed {
            id: 9,
            tag: "tag ✓".into(),
            filter,
            text: "a note".into(),
        }
    );
}

#[tokio::test]
async fn a_path_parameter_of_a_struct_type_is_refused_when_it_is_called() {
    let (url, api) = served().await;

    // The server cannot read it: a 500, logged, with no detail sent.
    let response = reqwest::get(format!("{url}/api/points/x"))
        .await
        .expect("the server answers");
    let status = response.status();
    let problem: Value = response.json().await.expect("the body is JSON");

    assert_eq!(status, StatusCode::INTERNAL_SERVER_ERROR);
    assert_eq!(problem.get("detail"), None);

    // The client cannot write it: an error with no status, before anything is sent.
    let error = api
        .points()
        .point(Point { x: 1 })
        .await
        .expect_err("a struct is not a segment");

    assert_eq!(error.status(), None);
    assert!(
        error
            .to_string()
            .contains("the path parameter `point` of `points_point` is not written as text"),
        "{error}"
    );
}
