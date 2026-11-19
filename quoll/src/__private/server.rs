//! The server side of what `rest::api!` writes: the routes of a group on an axum router, the caller authenticated and
//! the arguments read from a request, and the answers, an error answered as an RFC 9457 problem.

use std::error::Error as StdError;
use std::future::Future;
use std::sync::Arc;

use axum::Router;
use axum::body::Body;
use axum::extract::rejection::JsonRejection;
use axum::extract::{FromRequest, FromRequestParts, Path};
use axum::response::{IntoResponse, Response};
use axum::routing::on;
use http::request::Parts;
use http::{HeaderMap, HeaderValue, StatusCode, header};
use serde::Serialize;
use serde::de::DeserializeOwned;

use crate::__private::Given;
use crate::rest::error::Repr;
use crate::rest::{Authenticate, Created, Error, Result, Scheme};

pub use axum::routing::MethodFilter;

/// The routes of one group, each served by the group's service.
pub struct Group<S> {
    router: Router,
    service: Arc<S>,
}

impl<S: Send + Sync + 'static> Group<S> {
    /// Starts adding the routes of `service` to `router`.
    pub fn new(router: Router, service: S) -> Self {
        Self {
            router,
            service: Arc::new(service),
        }
    }

    /// Serves `method` on `path` with `handler`, given the service and the request. An error it returns is answered as
    /// a problem; a 401 carries a `WWW-Authenticate` challenge for each scheme of `C`, the schemes the route takes,
    /// as RFC 9110 asks, and none on a route that takes no scheme, `()`.
    ///
    /// # Panics
    ///
    /// When axum refuses the path, which the macro has checked it would not.
    pub fn route<C, F, Fut>(self, method: MethodFilter, path: &str, handler: F) -> Self
    where
        C: Challenges,
        F: Fn(Arc<S>, Request) -> Fut + Clone + Send + Sync + 'static,
        Fut: Future<Output = Result<Response>> + Send + 'static,
    {
        let service = Arc::clone(&self.service);
        let endpoint = on(method, move |request: axum::extract::Request| {
            let answer = handler(Arc::clone(&service), Request::new(request));

            async move {
                answer
                    .await
                    .unwrap_or_else(|error| challenged::<C>(error.into_response()))
            }
        });

        Self {
            router: self.router.route(path, endpoint),
            service: self.service,
        }
    }

    /// The router, with the group's routes.
    pub fn finish(self) -> Router {
        self.router
    }
}

/// The schemes a route takes, whose challenges its 401 carries: `()` for none, or a scheme the crate gives.
pub trait Challenges: 'static {
    /// Adds a `WWW-Authenticate` per scheme to the headers of a 401.
    fn challenge(headers: &mut HeaderMap);
}

impl Challenges for () {
    fn challenge(_headers: &mut HeaderMap) {}
}

impl<S: Given + 'static> Challenges for S {
    fn challenge(headers: &mut HeaderMap) {
        headers.append(header::WWW_AUTHENTICATE, S::CHALLENGE);
    }
}

/// A 401 with the challenges of the schemes `C`: a client knows so which credential to send.
fn challenged<C: Challenges>(mut response: Response) -> Response {
    if response.status() == StatusCode::UNAUTHORIZED {
        C::challenge(response.headers_mut());
    }

    response
}

/// A request, from which a handler reads its caller, then its arguments: the path, the query, then the body.
pub struct Request {
    parts: Parts,
    body: Body,
}

impl Request {
    fn new(request: axum::extract::Request) -> Self {
        let (parts, body) = request.into_parts();

        Self { parts, body }
    }

    /// The caller `service` makes of the credential of the scheme `S` the request carries. Only the headers are read:
    /// a request refused never has its body read.
    ///
    /// # Errors
    ///
    /// A 401 when the request carries no credential of `S`, or one that is not well formed; the error of `service`'s
    /// `authenticate` as it is, a 401 for a credential it does not know or a 500 for its own failure.
    pub fn authenticate<'a, S, A>(
        &self,
        service: &'a A,
    ) -> impl Future<Output = Result<A::Caller>> + Send + use<'a, S, A>
    where
        S: Scheme,
        S::Credential: Send,
        A: Authenticate<S>,
    {
        // Read now, so that the future does not borrow the request, whose body is not `Sync`.
        let credential = S::read(&self.parts);

        async move {
            let Some(credential) = credential? else {
                return Err(Error::unauthorized(
                    "the request carries no credential, which this route takes",
                ));
            };

            service.authenticate(credential).await
        }
    }

    /// The path's parameters, as axum reads them into `T`: the struct the macro writes for the route, a field per
    /// parameter, which leaves out the parameters of a router the api's is nested in.
    ///
    /// # Errors
    ///
    /// A 400 when a parameter does not read as its type.
    pub async fn path<T: DeserializeOwned + Send>(&mut self) -> Result<T> {
        Path::<T>::from_request_parts(&mut self.parts, &())
            .await
            .map(|Path(params)| params)
            .map_err(|error| rejected(error.status(), error.body_text()))
    }

    /// The query, read as serde_html_form reads a form into `T`: the struct the macro writes for the route, a field
    /// per key. An `Option` may be missing; an empty value, `?limit=`, is `None` for a number or a `bool` and `Some("")`
    /// for a string. A `Vec` takes every value of its key, and is empty when the key is missing.
    ///
    /// # Errors
    ///
    /// A 400 when the query does not read as `T`, or a value is not UTF-8 once decoded.
    pub fn query<T: DeserializeOwned>(&self) -> Result<T> {
        let query = self.parts.uri.query().unwrap_or_default();

        // serde_html_form decodes a value that is not UTF-8 with U+FFFD in it, which would reach the handler as text
        // the caller never sent. `&` and `=` are ASCII, so the whole query decodes as UTF-8 when each value does.
        if percent_encoding::percent_decode_str(query)
            .decode_utf8()
            .is_err()
        {
            return Err(Error::bad_request("the query is not UTF-8 once decoded"));
        }

        serde_html_form::from_str(query)
            .map_err(|error| Error::bad_request(format!("the query is not valid: {error}")))
    }

    /// The body, read as the JSON of `T`.
    ///
    /// # Errors
    ///
    /// A 400 when the body is not the JSON of `T`, a 415 when the request does not say it sends JSON, and the status
    /// axum gives when the body cannot be read, as a 413 for one too large.
    pub async fn json<T: DeserializeOwned>(self) -> Result<T> {
        let request = http::Request::from_parts(self.parts, self.body);

        match axum::Json::<T>::from_request(request, &()).await {
            Ok(axum::Json(value)) => Ok(value),
            Err(JsonRejection::JsonDataError(error)) => Err(Error::bad_request(error.body_text())),
            Err(JsonRejection::JsonSyntaxError(error)) => {
                Err(Error::bad_request(error.body_text()))
            }
            Err(error) => Err(rejected(error.status(), error.body_text())),
        }
    }
}

/// What axum refused in a request: sent when the request is at fault, logged when it is not.
fn rejected(status: StatusCode, text: String) -> Error {
    if status.is_client_error() {
        return Error::new(status, text);
    }

    Error::internal(text)
}

/// A 200 with the JSON of what the handler returned.
///
/// # Errors
///
/// The handler's error, or a 500 when its value cannot be written as JSON.
pub fn ok<T: Serialize>(result: Result<T>) -> Result<Response> {
    json(StatusCode::OK, &result?)
}

/// A 201 with the JSON of what the handler created, and its `Location` when it has one.
///
/// # Errors
///
/// The handler's error, or a 500 when its value cannot be written as JSON or its location is not a header's value.
pub fn created<T: Serialize>(result: Result<Created<T>>) -> Result<Response> {
    let Created { location, value } = result?;
    let mut response = json(StatusCode::CREATED, &value)?;

    if let Some(location) = location {
        let location = HeaderValue::try_from(location).map_err(|error| {
            Error::internal(format!(
                "the location of a `Created` is not a header's value: {error}"
            ))
        })?;
        response.headers_mut().insert(header::LOCATION, location);
    }

    Ok(response)
}

/// A 204, with no body.
///
/// # Errors
///
/// The handler's error.
pub fn no_content(result: Result<()>) -> Result<Response> {
    result?;

    Ok(StatusCode::NO_CONTENT.into_response())
}

fn json<T: Serialize>(status: StatusCode, value: &T) -> Result<Response> {
    let body = serde_json::to_vec(value).map_err(Error::internal)?;
    let content_type = [(
        header::CONTENT_TYPE,
        HeaderValue::from_static("application/json"),
    )];

    Ok((status, content_type, body).into_response())
}

/// What the server answers for this error: the status and, when it is to be sent, the detail. Logs what is not
/// sent.
fn answer(error: &Error) -> (StatusCode, Option<&str>) {
    match &*error.0 {
        Repr::Problem { status, detail }
            if status.is_client_error() || status.is_server_error() =>
        {
            (*status, Some(detail))
        }
        Repr::Problem { status, detail } => {
            tracing::error!(%status, detail, "a handler's error has a status that is not an error, answered 500");

            (StatusCode::INTERNAL_SERVER_ERROR, None)
        }
        Repr::Internal(source) => {
            tracing::error!(
                error = &**source as &dyn StdError,
                "internal error, answered 500"
            );

            (StatusCode::INTERNAL_SERVER_ERROR, None)
        }
        Repr::Upstream { status, .. } => {
            tracing::error!(%status, "another api answered an error, answered 502");

            (StatusCode::BAD_GATEWAY, None)
        }
        Repr::Failed {
            message,
            source,
            answered,
        } => {
            match source {
                Some(source) => tracing::error!(
                    error = &**source as &dyn StdError,
                    %answered,
                    "{message}"
                ),
                None => tracing::error!(%answered, "{message}"),
            }

            (*answered, None)
        }
    }
}

/// The RFC 9457 problem the server answers for an error.
#[derive(Serialize)]
struct Problem<'a> {
    #[serde(rename = "type")]
    kind: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    title: Option<&'static str>,
    status: u16,
    #[serde(skip_serializing_if = "Option::is_none")]
    detail: Option<&'a str>,
}

/// An error answered as an RFC 9457 problem, `application/problem+json`. An internal error and one received from
/// another API are logged through `tracing`, and their status alone is sent: 500 and 502.
impl IntoResponse for Error {
    fn into_response(self) -> Response {
        let (status, detail) = answer(&self);
        let problem = Problem {
            kind: "about:blank",
            title: status.canonical_reason(),
            status: status.as_u16(),
            detail,
        };
        let body = serde_json::to_vec(&problem)
            .expect("a problem holds strings and a number, which JSON always writes");
        let content_type = [(
            header::CONTENT_TYPE,
            HeaderValue::from_static("application/problem+json"),
        )];

        (status, content_type, body).into_response()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug, PartialEq, serde::Deserialize)]
    struct Search {
        page: u32,
        contains: Option<String>,
        limit: Option<u32>,
        #[serde(rename = "tag", default)]
        tags: Vec<String>,
    }

    fn query(text: &str) -> Result<Search> {
        let request = http::Request::builder()
            .uri(format!("/notes?{text}"))
            .body(Body::empty())
            .expect("the request is valid");

        Request::new(request).query()
    }

    #[test]
    fn a_query_reads_by_key() {
        let search = query("page=2&contains=milk%20jar&tag=a&tag=b").expect("a valid query");

        assert_eq!(
            search,
            Search {
                page: 2,
                contains: Some("milk jar".into()),
                limit: None,
                tags: vec!["a".into(), "b".into()],
            }
        );
    }

    #[test]
    fn a_missing_key_is_none_for_an_option_and_empty_for_a_vec() {
        let search = query("page=1").expect("a valid query");

        assert_eq!(search.contains, None);
        assert_eq!(search.tags, Vec::<String>::new());
        assert_eq!(
            query("contains=milk")
                .expect_err("`page` is needed")
                .status(),
            Some(StatusCode::BAD_REQUEST)
        );
    }

    #[test]
    fn an_empty_value_is_none_for_a_number_and_empty_for_a_string() {
        let search = query("page=1&contains=&limit=").expect("a valid query");

        assert_eq!(search.contains.as_deref(), Some(""));
        assert_eq!(search.limit, None);
    }

    #[test]
    fn a_value_that_is_not_its_type_is_a_bad_request() {
        let error = query("page=two").expect_err("`two` is not a number");

        assert_eq!(error.status(), Some(StatusCode::BAD_REQUEST));
    }

    #[test]
    fn a_value_that_is_not_utf8_is_a_bad_request() {
        let error = query("page=1&contains=%FF").expect_err("`%FF` is not UTF-8");

        assert_eq!(error.status(), Some(StatusCode::BAD_REQUEST));
    }

    /// What `answer` logs for `error`, and what it answers.
    fn logged(error: &Error) -> (String, StatusCode, Option<String>) {
        #[derive(Clone, Default)]
        struct Log(Arc<std::sync::Mutex<Vec<u8>>>);

        impl std::io::Write for Log {
            fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
                self.0
                    .lock()
                    .expect("no writer panics")
                    .extend_from_slice(bytes);
                Ok(bytes.len())
            }

            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }

        let log = Log::default();
        let writer = log.clone();
        let subscriber = tracing_subscriber::fmt()
            .with_writer(move || writer.clone())
            .with_ansi(false)
            .finish();

        let (status, detail) = tracing::subscriber::with_default(subscriber, || answer(error));
        let text = String::from_utf8(log.0.lock().expect("no writer panics").clone())
            .expect("the log is text");

        (text, status, detail.map(str::to_owned))
    }

    #[test]
    fn an_internal_error_is_logged_and_not_sent() {
        let (log, status, detail) = logged(&Error::internal("the database is down"));

        assert!(log.contains("the database is down"), "{log}");
        assert_eq!(status, StatusCode::INTERNAL_SERVER_ERROR);
        assert_eq!(detail, None);
    }

    #[test]
    fn an_error_from_another_api_is_logged_with_its_status() {
        let (log, status, detail) =
            logged(&Error::upstream(StatusCode::NOT_FOUND, None, Vec::new()));

        assert!(log.contains("404"), "{log}");
        assert_eq!(status, StatusCode::BAD_GATEWAY);
        assert_eq!(detail, None);
    }

    #[test]
    fn a_call_that_got_no_answer_is_logged_and_answers_its_status() {
        let error = Error::failed(
            "the call `notes_get` got no response",
            Some("connection refused".into()),
            StatusCode::BAD_GATEWAY,
        );
        let (log, status, detail) = logged(&error);

        assert!(log.contains("`notes_get` got no response"), "{log}");
        assert!(log.contains("connection refused"), "{log}");
        assert_eq!(status, StatusCode::BAD_GATEWAY);
        assert_eq!(detail, None);
    }

    #[test]
    fn a_problem_is_sent_and_not_logged() {
        let (log, status, detail) = logged(&Error::not_found("no such note"));

        assert_eq!(log, "");
        assert_eq!(status, StatusCode::NOT_FOUND);
        assert_eq!(detail.as_deref(), Some("no such note"));
    }

    #[tokio::test]
    async fn an_error_received_from_another_api_answers_502_without_its_body() {
        let router = Group::new(Router::new(), ())
            .route::<(), _, _>(MethodFilter::GET, "/upstream", |_, _| async {
                Err(Error::upstream(
                    StatusCode::NOT_FOUND,
                    None,
                    br#"{"detail":"the other api's secret"}"#.to_vec(),
                ))
            })
            .finish();
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
            .await
            .expect("a port is free");
        let address = listener.local_addr().expect("the listener has an address");
        let server = tokio::spawn(async move { axum::serve(listener, router).await });

        let response = reqwest::get(format!("http://{address}/upstream"))
            .await
            .expect("the server answers");
        let content_type = response.headers()[header::CONTENT_TYPE].clone();
        let status = response.status();
        let problem: serde_json::Value = response.json().await.expect("the body is JSON");

        assert_eq!(status, StatusCode::BAD_GATEWAY);
        assert_eq!(content_type, "application/problem+json");
        assert_eq!(
            problem,
            serde_json::json!({ "type": "about:blank", "title": "Bad Gateway", "status": 502 })
        );
        server.abort();
    }
}
