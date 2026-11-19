//! The server side of what `rest::api!` writes: the routes of a group on an axum router, the arguments read from a
//! request, and the answers, an error answered as an RFC 9457 problem.

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
use http::{HeaderValue, StatusCode, header};
use serde::de::{self, DeserializeOwned, Visitor, value::SeqDeserializer};
use serde::{Deserialize, Serialize};

use crate::rest::error::Repr;
use crate::rest::{Created, Error, Result};

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
    /// a problem.
    ///
    /// # Panics
    ///
    /// When axum refuses the path, which the macro has checked it would not.
    pub fn route<F, Fut>(self, method: MethodFilter, path: &str, handler: F) -> Self
    where
        F: Fn(Arc<S>, Request) -> Fut + Clone + Send + Sync + 'static,
        Fut: Future<Output = Result<Response>> + Send + 'static,
    {
        let service = Arc::clone(&self.service);
        let endpoint = on(method, move |request: axum::extract::Request| {
            let answer = handler(Arc::clone(&service), Request::new(request));

            async move { answer.await.unwrap_or_else(IntoResponse::into_response) }
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

/// A request, from which a handler reads its arguments: the path, then the query, then the body.
pub struct Request {
    parts: Parts,
    body: Body,
}

impl Request {
    fn new(request: axum::extract::Request) -> Self {
        let (parts, body) = request.into_parts();

        Self { parts, body }
    }

    /// The path's parameters, as axum reads them: a tuple, in the order of the path.
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

    /// The pairs of the query, from which each argument is read by its key.
    pub fn query(&self) -> Query {
        let query = self.parts.uri.query().unwrap_or_default();
        let pairs = form_urlencoded::parse(query.as_bytes())
            .map(|(key, value)| (key.into_owned(), value.into_owned()))
            .collect();

        Query(pairs)
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

/// The pairs of a query.
pub struct Query(Vec<(String, String)>);

impl Query {
    /// The argument under `key`, read as serde_html_form reads a field: an `Option` may be missing, a `Vec` takes every
    /// value of the key, and any other type takes one.
    ///
    /// # Errors
    ///
    /// A 400 when the key is missing and `T` needs it, or its value does not read as `T`.
    pub fn get<T: DeserializeOwned>(&self, key: &str) -> Result<T> {
        let mut values = self
            .0
            .iter()
            .filter(|(name, _)| name == key)
            .map(|(_, value)| value.as_str())
            .peekable();

        if values.peek().is_none() {
            return T::deserialize(Absent).map_err(|_| {
                Error::bad_request(format!("the query has no `{key}`, which this route needs"))
            });
        }

        let form = form_urlencoded::Serializer::new(String::new())
            .extend_pairs(values.map(|value| ("value", value)))
            .finish();

        serde_html_form::from_str::<One<T>>(&form)
            .map(|one| one.value)
            .map_err(|error| {
                Error::bad_request(format!("the query's `{key}` is not valid: {error}"))
            })
    }
}

/// The values of one key, read as the one field of a form.
#[derive(Deserialize)]
struct One<T> {
    value: T,
}

/// A key the query does not have: `None` for an `Option`, empty for a `Vec`, and an error for any other type.
struct Absent;

impl<'de> de::Deserializer<'de> for Absent {
    type Error = de::value::Error;

    fn deserialize_any<V: Visitor<'de>>(self, _visitor: V) -> Result<V::Value, Self::Error> {
        Err(de::Error::custom("missing"))
    }

    fn deserialize_option<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
        visitor.visit_none()
    }

    fn deserialize_seq<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
        visitor.visit_seq(SeqDeserializer::new(std::iter::empty::<()>()))
    }

    serde::forward_to_deserialize_any! {
        bool i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 char str string bytes byte_buf unit unit_struct
        newtype_struct tuple tuple_struct map struct enum identifier ignored_any
    }
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

    fn query(text: &str) -> Query {
        let request = http::Request::builder()
            .uri(format!("/notes?{text}"))
            .body(Body::empty())
            .expect("the request is valid");

        Request::new(request).query()
    }

    fn status_of<T: std::fmt::Debug>(result: Result<T>) -> Option<StatusCode> {
        result.expect_err("the value is refused").status()
    }

    #[test]
    fn a_query_key_reads_as_its_type() {
        let query = query("page=2&contains=milk%20jar&tag=a&tag=b");

        assert_eq!(query.get::<u32>("page").ok(), Some(2));
        assert_eq!(
            query.get::<Option<String>>("contains").ok(),
            Some(Some("milk jar".into()))
        );
        assert_eq!(
            query.get::<Vec<String>>("tag").ok(),
            Some(vec!["a".into(), "b".into()])
        );
    }

    #[test]
    fn a_missing_key_is_none_for_an_option_and_empty_for_a_vec() {
        let query = query("other=1");

        assert_eq!(query.get::<Option<u32>>("page").ok(), Some(None));
        assert_eq!(query.get::<Vec<u32>>("tag").ok(), Some(Vec::new()));
        assert_eq!(
            status_of(query.get::<u32>("page")),
            Some(StatusCode::BAD_REQUEST)
        );
    }

    #[test]
    fn a_value_that_is_not_its_type_is_a_bad_request() {
        let query = query("page=two");

        let error = query.get::<u32>("page").expect_err("`two` is not a number");

        assert_eq!(error.status(), Some(StatusCode::BAD_REQUEST));
        assert!(error.to_string().contains("`page`"), "{error}");
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
        let (log, status, detail) = logged(&Error::upstream(StatusCode::NOT_FOUND, Vec::new()));

        assert!(log.contains("404"), "{log}");
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
            .route(MethodFilter::GET, "/upstream", |_, _| async {
                Err(Error::upstream(
                    StatusCode::NOT_FOUND,
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
