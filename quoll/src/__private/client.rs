//! The client side of what `rest::api!` writes: a request built from a route's arguments, sent through
//! reqwest-middleware with its [`Route`] in its extensions, and its answer read as the route's result or as an error.

use std::error::Error as StdError;
use std::fmt;
use std::sync::Arc;

use http::{HeaderMap, HeaderValue, StatusCode, header};
use percent_encoding::{AsciiSet, NON_ALPHANUMERIC, utf8_percent_encode};
use reqwest::Url;
use reqwest_middleware::ClientWithMiddleware;
use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::Value;

use crate::rest::{Error, Problem, Result, Route};

pub use http::Method;

/// What a path parameter keeps as it is: the unreserved characters of RFC 3986. Every other byte is escaped, `/`, `?`
/// and `%` included, so that a value is one segment and reads back as it was written.
const SEGMENT: &AsciiSet = &NON_ALPHANUMERIC
    .remove(b'-')
    .remove(b'.')
    .remove(b'_')
    .remove(b'~');

/// How much of a body the client reads when the body is not the route's result: an error's, which the error keeps,
/// and a success's on a route with no result, read so that the connection serves again. Past it, the client reads no
/// further.
const BODY_LIMIT: usize = 64 * 1024;

/// The route a request calls, which the macro writes as a `static` per route.
pub const fn route(
    operation_id: &'static str,
    group: &'static str,
    method: Method,
    path: &'static str,
    schemes: &'static [&'static str],
) -> Route {
    Route {
        operation_id,
        group,
        method,
        path,
        schemes,
    }
}

/// A client of an api: the HTTP client and the URL the api is served at. Its clones share both.
#[derive(Clone)]
pub struct Client(Arc<Inner>);

struct Inner {
    http: ClientWithMiddleware,
    base: Url,
}

impl Client {
    /// A client of the api at `url`, with no middleware.
    ///
    /// # Errors
    ///
    /// When `url` is not an `http` or `https` URL with no query, or reqwest cannot build its client.
    pub fn new(url: &str) -> Result<Self> {
        let http = reqwest::Client::builder().build().map_err(|error| {
            Error::misuse("the http client cannot be built", Some(Box::new(error)))
        })?;

        Self::with(http.into(), url)
    }

    /// A client of the api at `url`, sending its requests through `http`.
    ///
    /// The messages of its errors, which a handler logs, never show the credentials of `url`: a URL that does not
    /// parse is not shown at all.
    ///
    /// # Errors
    ///
    /// When `url` is not an `http` or `https` URL with no query.
    pub fn with(http: ClientWithMiddleware, url: &str) -> Result<Self> {
        let base = Url::parse(url).map_err(|error| {
            Error::misuse(
                "the url given is not the url of an api",
                Some(Box::new(error)),
            )
        })?;

        if !matches!(base.scheme(), "http" | "https") {
            return Err(Error::misuse(
                format!(
                    "the url of an api is `http` or `https`, not `{}`: write it as `http://host:port`",
                    base.scheme(),
                ),
                None,
            ));
        }
        if base.query().is_some() || base.fragment().is_some() {
            return Err(Error::misuse(
                format!(
                    "`{}` is not the url of an api: it has a query or a fragment, which each route replaces",
                    redacted(&base),
                ),
                None,
            ));
        }

        Ok(Self(Arc::new(Inner { http, base })))
    }
}

/// Shows where the api is, without the credentials a URL may hold.
impl fmt::Debug for Client {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Client")
            .field("url", &redacted(&self.0.base))
            .finish()
    }
}

/// An `http` or `https` URL without its credentials, its query and its fragment: `http://localhost:8080/v1`.
fn redacted(url: &Url) -> String {
    format!("{}{}", url.origin().ascii_serialization(), url.path())
}

/// One call of a route: its path, its query and its body, written from the route's arguments, then sent.
#[derive(Debug)]
pub struct Call<'a> {
    client: &'a Client,
    route: &'static Route,
    path: String,
    query: Option<String>,
    body: Option<Vec<u8>>,
}

impl<'a> Call<'a> {
    /// Starts a call of `route` through `client`.
    pub fn new(client: &'a Client, route: &'static Route) -> Self {
        Self {
            client,
            route,
            path: String::new(),
            query: None,
            body: None,
        }
    }

    /// Adds `text` to the path as it is: what the contract writes between its parameters, `/api/notes`.
    #[must_use]
    pub fn path(mut self, text: &'static str) -> Self {
        self.path.push_str(text);
        self
    }

    /// Adds a `/` and `value` to the path, as one segment: the text serde writes for it, escaped.
    ///
    /// # Errors
    ///
    /// When `value` is not written as text, a number or a `bool`, or is a segment a URL cannot hold: empty, `.` or
    /// `..`, which would call another route.
    pub fn segment<T: Serialize + ?Sized>(mut self, name: &str, value: &T) -> Result<Self> {
        let operation_id = self.route.operation_id;
        let text = match serde_json::to_value(value) {
            Ok(Value::String(text)) => text,
            Ok(Value::Number(number)) => number.to_string(),
            Ok(Value::Bool(value)) => value.to_string(),
            Ok(_) | Err(_) => {
                return Err(Error::misuse(
                    format!(
                        "the path parameter `{name}` of `{operation_id}` is not written as text"
                    ),
                    None,
                ));
            }
        };

        if matches!(text.as_str(), "" | "." | "..") {
            return Err(Error::misuse(
                format!(
                    "the path parameter `{name}` of `{operation_id}` is `{text}`, which is not a segment of a path: it would call another route"
                ),
                None,
            ));
        }

        self.path.push('/');
        self.path.extend(utf8_percent_encode(&text, SEGMENT));
        Ok(self)
    }

    /// Sets the query to `query`, written as serde_html_form writes a form: a `None` leaves its key out, a `Vec`
    /// repeats it.
    ///
    /// # Errors
    ///
    /// When serde_html_form cannot write `query`.
    pub fn query<T: Serialize>(mut self, query: &T) -> Result<Self> {
        let query = serde_html_form::to_string(query).map_err(|error| {
            Error::misuse(
                format!(
                    "the query of `{}` cannot be written",
                    self.route.operation_id
                ),
                Some(Box::new(error)),
            )
        })?;

        self.query = Some(query).filter(|query| !query.is_empty());
        Ok(self)
    }

    /// Sets the body to the JSON of `body`.
    ///
    /// # Errors
    ///
    /// When `body` cannot be written as JSON.
    pub fn json<T: Serialize + ?Sized>(mut self, body: &T) -> Result<Self> {
        let body = serde_json::to_vec(body).map_err(|error| {
            Error::misuse(
                format!(
                    "the body of `{}` cannot be written as JSON",
                    self.route.operation_id
                ),
                Some(Box::new(error)),
            )
        })?;

        self.body = Some(body);
        Ok(self)
    }

    /// Sends the request, with the route in its extensions, and keeps an answer with a success status.
    ///
    /// # Errors
    ///
    /// The answer with its status, its problem and the first 64 KiB of its body, when its status is not a success;
    /// an error with no status when the request got no response or the body of an error could not be read.
    pub async fn send(self) -> Result<Answer> {
        let Self {
            client,
            route,
            path,
            query,
            body,
        } = self;
        let Inner { http, base } = &*client.0;
        let mut url = base.clone();
        url.set_path(&format!("{}{path}", base.path().trim_end_matches('/')));
        url.set_query(query.as_deref());

        let mut request = http
            .request(route.method.clone(), url)
            .with_extension(route.clone());
        if let Some(body) = body {
            request = request
                .header(
                    header::CONTENT_TYPE,
                    HeaderValue::from_static("application/json"),
                )
                .body(body);
        }

        let response = request
            .send()
            .await
            .map_err(|error| no_answer(route, "got no response", error))?;
        let status = response.status();
        if status.is_success() {
            return Ok(Answer { response, route });
        }

        let is_problem = is_problem(response.headers());
        let body = read_up_to_limit(response)
            .await
            .map_err(|error| no_answer(route, "got an error whose body cannot be read", error))?;
        let problem = if is_problem {
            read_problem(&body)
        } else {
            None
        };

        Err(Error::upstream(status, problem, body))
    }
}

/// The error of a call that got no answer to read: a handler that returns it answers 502.
fn no_answer(route: &Route, what: &str, error: impl StdError + Send + Sync + 'static) -> Error {
    Error::failed(
        format!("the call `{}` {what}", route.operation_id),
        Some(Box::new(error)),
        StatusCode::BAD_GATEWAY,
    )
}

/// An answer with a success status, read as the route's result.
#[derive(Debug)]
pub struct Answer {
    response: reqwest::Response,
    route: &'static Route,
}

impl Answer {
    /// The body, read as the JSON of `T`: what a route returning `T` or `Created<T>` gives. It is read whole: it is
    /// the route's data.
    ///
    /// # Errors
    ///
    /// An error with no status when the body cannot be read, or is not the JSON of `T`.
    pub async fn json<T: DeserializeOwned>(self) -> Result<T> {
        let Self { response, route } = self;
        let body = response
            .bytes()
            .await
            .map_err(|error| no_answer(route, "got an answer whose body cannot be read", error))?;

        serde_json::from_slice(&body)
            .map_err(|error| no_answer(route, "got an answer that is not its result", error))
    }

    /// Nothing: what a route with no result gives, whatever the body. A body is read and dropped, up to 64 KiB, so
    /// that the connection serves the next call.
    ///
    /// # Errors
    ///
    /// None: a success status is all such a route answers.
    pub async fn none(self) -> Result<()> {
        // A body past the limit, or one that fails, only costs the connection: the call itself has succeeded.
        let _ = read_up_to_limit(self.response).await;

        Ok(())
    }
}

/// The body of `response`, read up to [`BODY_LIMIT`] bytes: past them, the rest is not read.
async fn read_up_to_limit(mut response: reqwest::Response) -> reqwest::Result<Vec<u8>> {
    let mut body = Vec::new();

    while body.len() < BODY_LIMIT {
        let Some(chunk) = response.chunk().await? else {
            break;
        };
        let room = BODY_LIMIT - body.len();
        body.extend_from_slice(&chunk[..chunk.len().min(room)]);
    }

    Ok(body)
}

/// Whether the content type of an answer is `application/problem+json`, whatever its parameters.
fn is_problem(headers: &HeaderMap) -> bool {
    headers
        .get(header::CONTENT_TYPE)
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.split(';').next())
        .is_some_and(|essence| {
            essence
                .trim()
                .eq_ignore_ascii_case("application/problem+json")
        })
}

/// The RFC 9457 problem in a body: a JSON object, whose members of the wrong type are left out.
fn read_problem(body: &[u8]) -> Option<Problem> {
    let Ok(Value::Object(members)) = serde_json::from_slice::<Value>(body) else {
        return None;
    };
    let text = |name: &str| members.get(name).and_then(Value::as_str).map(str::to_owned);
    let status = members
        .get("status")
        .and_then(Value::as_u64)
        .and_then(|status| u16::try_from(status).ok())
        .and_then(|status| StatusCode::from_u16(status).ok());

    Some(Problem {
        kind: text("type").unwrap_or_else(|| "about:blank".to_owned()),
        title: text("title"),
        status,
        detail: text("detail"),
        instance: text("instance"),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    static ROUTE: Route = route("notes_get", "Notes", Method::GET, "/api/notes/{id}", &[]);

    fn client(url: &str) -> Result<Client> {
        Client::new(url)
    }

    fn segment<T: Serialize + ?Sized>(value: &T) -> Result<String> {
        let client = client("http://localhost").expect("a valid url");

        Call::new(&client, &ROUTE)
            .segment("id", value)
            .map(|call| call.path)
    }

    #[test]
    fn a_segment_escapes_all_but_the_unreserved_characters() {
        let cases = [
            ("a/b", "/a%2Fb"),
            ("what?", "/what%3F"),
            ("50%", "/50%25"),
            ("milk jar", "/milk%20jar"),
            ("a+b&c=d#e", "/a%2Bb%26c%3Dd%23e"),
            ("é", "/%C3%A9"),
            ("v1.2-beta_3~", "/v1.2-beta_3~"),
            (".hidden", "/.hidden"),
        ];

        for (value, path) in cases {
            assert_eq!(segment(value).expect("a segment").as_str(), path, "{value}");
        }
    }

    #[test]
    fn a_segment_is_the_text_serde_writes() {
        #[derive(Serialize)]
        #[serde(rename_all = "snake_case")]
        enum Shelf {
            KitchenTop,
        }

        assert_eq!(segment(&-3_i64).expect("a number"), "/-3");
        assert_eq!(segment(&true).expect("a bool"), "/true");
        assert_eq!(
            segment(&Shelf::KitchenTop).expect("a unit variant"),
            "/kitchen_top"
        );
    }

    #[test]
    fn a_value_that_would_call_another_route_is_refused() {
        for value in ["", ".", ".."] {
            let error = segment(value).expect_err("not a segment");

            assert_eq!(error.status(), None);
            assert!(error.to_string().contains("not a segment"), "{error}");
        }
        assert!(
            segment(&[1, 2])
                .expect_err("not text")
                .to_string()
                .contains("not written as text")
        );
    }

    #[test]
    fn a_query_leaves_none_out_and_repeats_a_vec() {
        #[derive(Serialize)]
        struct Query {
            contains: Option<String>,
            page: Option<u32>,
            tag: Vec<String>,
        }
        let client = client("http://localhost").expect("a valid url");
        let query = |query: &Query| {
            Call::new(&client, &ROUTE)
                .query(query)
                .expect("a query")
                .query
        };

        let full = Query {
            contains: Some("milk jar".into()),
            page: Some(2),
            tag: vec!["a".into(), "b&c".into()],
        };
        let empty = Query {
            contains: None,
            page: None,
            tag: Vec::new(),
        };

        assert_eq!(
            query(&full).as_deref(),
            Some("contains=milk+jar&page=2&tag=a&tag=b%26c")
        );
        assert_eq!(query(&empty), None);
    }

    #[test]
    fn a_url_that_is_not_an_apis_is_an_error_with_no_status() {
        let cases = [
            ("", "not the url of an api"),
            ("localhost:8080", "`http` or `https`, not `localhost`"),
            ("ftp://localhost", "`http` or `https`, not `ftp`"),
            ("http://localhost/?page=1", "has a query or a fragment"),
            ("http://localhost/#top", "has a query or a fragment"),
        ];

        for (url, message) in cases {
            let error = client(url).expect_err("not the url of an api");

            assert_eq!(error.status(), None, "{url}");
            assert!(error.to_string().contains(message), "{url}: {error}");
        }
        assert!(client("https://example.com/v1/").is_ok());
    }

    #[test]
    fn an_error_about_a_url_does_not_show_its_credentials() {
        for url in [
            "http://ana:secret@local host/",
            "ftp://ana:secret@localhost/",
            "http://ana:secret@localhost/?page=1",
        ] {
            let error = client(url).expect_err("not the url of an api");
            let shown = format!("{error} {error:?}");

            assert!(
                !shown.contains("secret") && !shown.contains("ana"),
                "{shown}"
            );
        }
    }

    #[test]
    fn a_client_does_not_show_the_credentials_of_its_url() {
        let client = client("http://ana:secret@localhost:8080/v1").expect("a valid url");

        assert_eq!(
            format!("{client:?}"),
            r#"Client { url: "http://localhost:8080/v1" }"#
        );
    }

    #[test]
    fn a_problem_is_read_leaving_out_members_of_the_wrong_type() {
        let body = br#"{"title": "Not Found", "status": 404, "detail": 7, "instance": "/notes/9", "code": 1}"#;
        let problem = read_problem(body).expect("an object");

        assert_eq!(problem.kind(), "about:blank");
        assert_eq!(problem.title(), Some("Not Found"));
        assert_eq!(problem.status(), Some(StatusCode::NOT_FOUND));
        assert_eq!(problem.detail(), None);
        assert_eq!(problem.instance(), Some("/notes/9"));
        assert_eq!(read_problem(b"[1]"), None);
        assert_eq!(read_problem(b"not json"), None);
        assert_eq!(
            read_problem(br#"{"status": 99}"#).and_then(|p| p.status()),
            None
        );
    }

    #[test]
    fn a_problem_is_known_by_its_content_type() {
        let headers = |value: &'static str| {
            HeaderMap::from_iter([(header::CONTENT_TYPE, HeaderValue::from_static(value))])
        };

        assert!(is_problem(&headers("application/problem+json")));
        assert!(is_problem(&headers(
            "Application/Problem+JSON; charset=utf-8"
        )));
        assert!(!is_problem(&headers("application/json")));
        assert!(!is_problem(&HeaderMap::new()));
    }
}
