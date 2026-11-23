//! The client side of what `rest::api!` writes: a request built from a route's arguments, sent through
//! reqwest-middleware with its [`Route`] in its extensions, and its answer read as the route's result, whole or as a
//! stream, or as an error.

use std::borrow::Cow;
use std::error::Error as StdError;
use std::fmt;
use std::future::{Future, IntoFuture};
use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;

use http::{HeaderMap, HeaderValue, StatusCode, header};
use percent_encoding::{AsciiSet, NON_ALPHANUMERIC, utf8_percent_encode};
use reqwest::Url;
use reqwest_middleware::ClientWithMiddleware;
use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::Value;

use crate::__private::content_type::{
    self, FORM, JSON_LINES, OCTET_STREAM, PROBLEM, TEXT_UTF8, media_type_value,
};
use crate::__private::headers::{Fixed, RouteHeaders};
use crate::__private::join::{self, Join};
use crate::__private::known::{IsByteStream, IsBytes, IsEvents, IsForm, IsJsonLines, IsText};
use crate::__private::read::{Deserialized, Read};
use crate::__private::text;
use crate::client::{CallTimeout, KeepsDeadline};
use crate::rest::{self, Error, Problem, Result, Route};

mod redact;
mod stream;

pub(crate) use redact::{redacted, strip_urls};

pub use crate::__private::content_type::{EVENT_STREAM, JSON, TEXT};
pub use http::HeaderName;

/// Any content type, which a route returning `Bytes` asks for.
pub const ANY: &str = "*/*";

/// The content types of JSON Lines the client reads: its own, the two names some APIs send it with, and JSON, which
/// others send it as, the Docker Engine API among them.
const JSON_LINES_READ: &[&str] = &[
    JSON_LINES,
    "application/x-ndjson",
    "application/x-jsonlines",
    JSON,
];

/// What a route returning `JsonLines` asks for: JSON Lines by its own name first, and JSON last.
pub const ANY_JSON_LINES: &str = "application/jsonl, application/x-ndjson;q=0.9, application/x-jsonlines;q=0.9, application/json;q=0.5";

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

/// The origin of the api a request of a contract calls, which the client puts in its extensions beside its [`Route`]:
/// the middleware that send a credential send it to that origin only, and so not on a redirect to another.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ApiOrigin(pub(crate) url::Origin);

/// A client of an api: the HTTP client and the URL the api is served at. Its clones share both.
#[derive(Clone)]
pub struct Client(Arc<Inner>);

struct Inner {
    http: ClientWithMiddleware,
    base: Url,
    origin: ApiOrigin,
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

        let origin = ApiOrigin(base.origin());

        Ok(Self(Arc::new(Inner { http, base, origin })))
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

/// The request of one call of a route: its path, its query, its headers and its body, written from the route's
/// arguments, then sent.
#[derive(Debug)]
pub struct Request<'a> {
    client: &'a Client,
    route: &'static Route,
    path: String,
    query: Option<String>,
    headers: HeaderMap,
    body: Option<Body>,
    timeout: Option<Duration>,
}

/// A body, with its content type.
#[derive(Debug)]
struct Body {
    content_type: HeaderValue,
    bytes: Vec<u8>,
}

impl<'a> Request<'a> {
    /// Starts a request of `route` through `client`.
    pub fn new(client: &'a Client, route: &'static Route) -> Self {
        Self {
            client,
            route,
            path: String::new(),
            query: None,
            headers: HeaderMap::new(),
            body: None,
            timeout: None,
        }
    }

    /// The request of `route` through `client`, as `write` writes it from the route's arguments.
    ///
    /// # Errors
    ///
    /// The error of `write`: an argument it cannot write.
    pub fn write<F>(client: &'a Client, route: &'static Route, write: F) -> Result<Self>
    where
        F: FnOnce(Self) -> Result<Self>,
    {
        write(Self::new(client, route))
    }

    /// Adds `text` to the path as it is: the api's prefix, `/api`, and what the contract writes between its parameters,
    /// `/notes`. A path left empty is `/`.
    #[must_use]
    pub fn path(mut self, text: &'static str) -> Self {
        self.path.push_str(text);
        self
    }

    /// Adds a `/` and `value` to the path, as one segment: the text serde writes for it, escaped.
    ///
    /// # Errors
    ///
    /// When `value` is not written as one piece of text, as a number, a string or a unit variant are; when it is a
    /// `None`; or when it is a segment a URL cannot hold: empty, `.` or `..`, which would call another route.
    pub fn segment<T: Serialize + ?Sized>(mut self, name: &str, value: &T) -> Result<Self> {
        let operation_id = self.route.operation_id;
        let text = match text::write::to_text(value) {
            Some(Some(text)) => text,
            Some(None) => {
                return Err(Error::misuse(
                    format!(
                        "the path parameter `{name}` of `{operation_id}` is `None`, which no segment of a path stands for"
                    ),
                    None,
                ));
            }
            None => {
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

    /// Adds the key `key` to the query, after what [`query`](Self::query) wrote, with `values` joined into its one
    /// value: each value's text escaped, as a path parameter's is, and the separator between them as it is,
    /// `labels=bug,docs`. An empty list leaves the key out, as the server reads it.
    ///
    /// # Errors
    ///
    /// When a value is not written as one piece of text, is a `None`, or holds the separator, which the server would
    /// read as two values; or when the list is one empty value, which the server would read as an empty list.
    pub fn join<T: Serialize>(mut self, key: &str, join: Join, values: &[T]) -> Result<Self> {
        let mut written = String::new();
        let any = join::write::each_text(join, values, |first, text| {
            // The separator as it is, which the URL escapes when it is a space, `%20`.
            if !first {
                written.push(join.separator());
            }
            written.extend(utf8_percent_encode(text, SEGMENT));
        })
        .map_err(|refused| {
            let what = format!("the query key `{key}` of `{}`", self.route.operation_id);

            Error::misuse(refused.message(join, &what), None)
        })?;
        if !any {
            return Ok(self);
        }

        let query = self.query.get_or_insert_default();
        if !query.is_empty() {
            query.push('&');
        }
        query.extend(utf8_percent_encode(key, SEGMENT));
        query.push('=');
        query.push_str(&written);
        Ok(self)
    }

    /// Sets the body to the JSON of `body`.
    ///
    /// # Errors
    ///
    /// When `body` cannot be written as JSON.
    pub fn json<T: Serialize>(mut self, body: T) -> Result<Self> {
        let body = serde_json::to_vec(&body).map_err(|error| {
            Error::misuse(
                format!(
                    "the body of `{}` cannot be written as JSON",
                    self.route.operation_id
                ),
                Some(Box::new(error)),
            )
        })?;

        self.body = Some(Body {
            content_type: HeaderValue::from_static(JSON),
            bytes: body,
        });
        Ok(self)
    }

    /// Sets the body to the `T` of `Form<T>`, written as a form, `application/x-www-form-urlencoded`, as the query
    /// is.
    ///
    /// # Errors
    ///
    /// When serde_html_form cannot write the form.
    pub fn form<F>(mut self, body: F) -> Result<Self>
    where
        F: IsForm,
        F::Value: Serialize,
    {
        let body = serde_html_form::to_string(body.value()).map_err(|error| {
            Error::misuse(
                format!(
                    "the body of `{}` cannot be written as a form",
                    self.route.operation_id
                ),
                Some(Box::new(error)),
            )
        })?;

        self.body = Some(Body {
            content_type: HeaderValue::from_static(FORM),
            bytes: body.into_bytes(),
        });
        Ok(self)
    }

    /// Sets the body to `body`, `text/plain` in UTF-8.
    #[must_use]
    pub fn text<T: IsText>(mut self, body: T) -> Self {
        self.body = Some(Body {
            content_type: HeaderValue::from_static(TEXT_UTF8),
            bytes: body.into_string().into_bytes(),
        });
        self
    }

    /// Sets the body to `body`, of its content type.
    ///
    /// # Errors
    ///
    /// When its content type is not a media type.
    pub fn bytes<B: IsBytes>(mut self, body: B) -> Result<Self> {
        let (bytes, content_type) = body.into_parts();
        let content_type = media_type_value(content_type).map_err(|content_type| {
            Error::misuse(
                format!(
                    "the body of `{}` has a content type that is not a media type: {content_type:?}",
                    self.route.operation_id
                ),
                None,
            )
        })?;

        self.body = Some(Body {
            content_type,
            bytes,
        });
        Ok(self)
    }

    /// Adds the header `name` with the text serde writes for `value`, as a path parameter's; a `None` leaves it out.
    ///
    /// # Errors
    ///
    /// When `value` is not written as one piece of text, or its text cannot be a header's value as it is: a control
    /// character, a line break, or a space or a tab at either end, which HTTP drops. The message does not show the
    /// value.
    pub fn header<T: Serialize + ?Sized>(mut self, name: HeaderName, value: &T) -> Result<Self> {
        let operation_id = self.route.operation_id;
        let Some(text) = text::write::to_text(value) else {
            return Err(Error::misuse(
                format!("the header `{name}` of `{operation_id}` is not written as text"),
                None,
            ));
        };
        let Some(text) = text else {
            return Ok(self);
        };

        let blank = |c: char| c == ' ' || c == '\t';
        if text.starts_with(blank) || text.ends_with(blank) {
            return Err(Error::misuse(
                format!(
                    "the header `{name}` of `{operation_id}` starts or ends with a space or a tab, which HTTP drops: it would not read back as it was written"
                ),
                None,
            ));
        }

        let value = HeaderValue::try_from(text).map_err(|_| {
            Error::misuse(
                format!(
                    "the header `{name}` of `{operation_id}` holds a character a header cannot, a control character or a line break"
                ),
                None,
            )
        })?;

        self.headers.append(name, value);
        Ok(self)
    }

    /// Adds the fixed headers of the route, its own and its group's, then the ones it takes of `api`'s, each unless an
    /// argument of the route already sent it: the caller's wins. A fixed `Content-Type` is the body's, whose media
    /// type the macro has checked it keeps, and is left out of a request with no body.
    #[must_use]
    pub fn fixed(mut self, api: &[Fixed], route: &RouteHeaders) -> Self {
        let mut content_type = false;

        for fixed in route.sent(api) {
            let name = HeaderName::from_bytes(fixed.name.as_bytes())
                .expect("the macro checks that a fixed header's name is a token");
            // Checked at compile time with `headers::is_value`, which takes what `from_static` takes.
            let value = HeaderValue::from_static(fixed.value);

            if name == header::CONTENT_TYPE {
                if let Some(body) = &mut self.body
                    && !content_type
                {
                    body.content_type = value;
                    content_type = true;
                }
                continue;
            }

            if !self.headers.contains_key(&name) {
                self.headers.insert(name, value);
            }
        }

        self
    }

    /// Asks for an answer of `media_type`, the route's result: what the `Accept` header says, unless the route sends
    /// one of its own.
    #[must_use]
    pub fn accept(mut self, media_type: &'static str) -> Self {
        if !self.headers.contains_key(header::ACCEPT) {
            self.headers
                .insert(header::ACCEPT, HeaderValue::from_static(media_type));
        }
        self
    }

    /// Sends the request, with the route in its extensions, and keeps an answer with a success status.
    ///
    /// # Errors
    ///
    /// The answer with its status, its detail of the shape `E` and the first 64 KiB of its body, when its status is
    /// not a success; an error with no status when the request got no response or the body of an error could not be
    /// read.
    async fn send<E>(self, shape: Shaped<E>) -> Result<Answer, E> {
        let route = self.route;
        let response = self.dispatch().await.map_err(Error::cast)?;

        if response.status().is_success() {
            return Ok(Answer { response, route });
        }

        Err(answered_error(response, route, shape).await?)
    }

    /// Sends the request, with the route in its extensions, and its deadline when the call has one, and gives the
    /// answer whatever its status.
    ///
    /// The deadline of a call is the request's own, which [`client::retry`](crate::client::retry) and
    /// [`client::redirects`](crate::client::redirects) copy, and the one [`client::timeout`](crate::client::timeout)
    /// takes in place of its own. On a stream, it bounds the head of the answer alone: `client::timeout`, which
    /// [`client::builder`](crate::client::builder) always puts, keeps it for each attempt, which the builder's chain
    /// says before the request is sent, with a mark on each request; a chain without the mark, built otherwise or with
    /// no middleware, has it kept here, for the whole call, unless a `client::timeout` of its own has kept it by then.
    ///
    /// # Errors
    ///
    /// An error with no status when the request got no response.
    async fn dispatch(self) -> Result<reqwest::Response> {
        let Self {
            client,
            route,
            path,
            query,
            headers,
            body,
            timeout,
        } = self;
        let Inner { http, base, origin } = &*client.0;
        let mut url = base.clone();
        // The route `"/"` of an api with no prefix.
        let path = if path.is_empty() { "/" } else { path.as_str() };
        url.set_path(&format!("{}{path}", base.path().trim_end_matches('/')));
        url.set_query(query.as_deref());

        let mut request = http
            .request(route.method.clone(), url)
            .headers(headers)
            .with_extension(route.clone())
            .with_extension(origin.clone());
        if let Some(Body {
            content_type,
            bytes,
        }) = body
        {
            request = request
                .header(header::CONTENT_TYPE, content_type)
                .body(bytes);
        }

        let Some(duration) = timeout else {
            return request
                .send()
                .await
                .map_err(|error| no_answer(route, "got no response", error));
        };

        let deadline = CallTimeout::new(duration);
        let kept = request.extensions().get::<KeepsDeadline>().is_some();

        request = request.with_extension(deadline.clone());

        if !route.streams {
            request = request.timeout(duration);
        }

        let sent = request.send();
        let answer = if route.streams && !kept {
            deadline.kept_or_bounding(sent).await
        } else {
            sent.await
        };

        answer.map_err(|error| no_answer(route, "got no response", error))
    }
}

/// The shape of the errors an api answers, which the client reads in the body of an answer with an error status: an
/// RFC 9457 [`Problem`], or the type an api, a group or a route names with `#[error(E)]`, which implements
/// `Deserialize`, `Display` and `Debug`, and is `Send`, `Sync` and `'static`.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not the shape of an api's errors",
    label = "`#[error(...)]` names a type that implements `Deserialize`, `Display` and `Debug`, and is `Send`, `Sync` and `'static`",
    note = "the client reads the body of an error answer as the JSON of the type, and its `Display` gives the error's message"
)]
pub trait Shape: fmt::Debug + Send + Sync + Sized + 'static {
    /// The detail an answer with `headers` and `body`, of an error status, holds, when it is one.
    fn read(headers: &HeaderMap, body: &[u8]) -> Option<Self>;

    /// What the error of the answer says after its status: `another api answered 401: Bad credentials`. `None` when
    /// it says nothing, so that the error says its status alone.
    fn message(&self) -> Option<String>;
}

/// A problem is read from `application/problem+json` alone, a JSON object whose members of the wrong type are left
/// out, and says its `detail`.
impl Shape for Problem {
    fn read(headers: &HeaderMap, body: &[u8]) -> Option<Self> {
        if !is_problem(headers) {
            return None;
        }

        read_problem(body)
    }

    fn message(&self) -> Option<String> {
        self.detail()
            .filter(|detail| !detail.is_empty())
            .map(str::to_owned)
    }
}

/// Any other shape is read as JSON, whatever the answer's content type, as APIs label their errors in many ways, and
/// says its `Display`, unless it is empty.
impl<E> Shape for E
where
    E: DeserializeOwned + fmt::Display + fmt::Debug + Send + Sync + 'static,
{
    fn read(_headers: &HeaderMap, body: &[u8]) -> Option<Self> {
        serde_json::from_slice(body).ok()
    }

    fn message(&self) -> Option<String> {
        Some(self.to_string()).filter(|message| !message.is_empty())
    }
}

/// How the client reads the errors of the shape `E`: the detail an answer holds, and what it says, as [`Shape`] has
/// them. The client of a group names one per shape its routes have, once, so that a type that is not a shape is one
/// error, on its `#[error(...)]`, rather than one per route, which a bound of each call would give.
pub struct Shaped<E> {
    read: fn(&HeaderMap, &[u8]) -> Option<E>,
    message: fn(&E) -> Option<String>,
}

/// The reading of `E`, which checks at compile time that it is the shape of an api's errors.
pub const fn shaped<E: Shape>() -> Shaped<E> {
    Shaped {
        read: E::read,
        message: E::message,
    }
}

impl<E> Clone for Shaped<E> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<E> Copy for Shaped<E> {}

/// The error of an answer with an error status, or a redirection: its status, its detail when its body is an `E`, and
/// the first 64 KiB of its body.
///
/// # Errors
///
/// An error with no status when the body cannot be read.
async fn answered_error<E>(
    response: reqwest::Response,
    route: &Route,
    shape: Shaped<E>,
) -> Result<Error<E>, E> {
    let status = response.status();
    let version = response.version();
    let headers = response.headers().clone();
    let body = read_up_to_limit(response).await.map_err(|error| {
        no_answer(route, "got an error whose body cannot be read", error).cast()
    })?;
    let detail = (shape.read)(&headers, &body);
    let message = detail.as_ref().and_then(shape.message);
    let answer = detail.map(|detail| rest::Response::from_parts(status, version, headers, detail));

    Ok(Error::upstream(status, answer, Some(body), message))
}

/// A call of a route, described and not sent yet: the request the route's arguments write, or why they could not,
/// and how its answer is read, its errors of the shape `E`. It is what each method of a group's client returns, a
/// [`rest::Call`].
#[must_use = "a call does nothing until it is awaited"]
pub struct Pending<'a, T, E = Problem> {
    request: Result<Request<'a>>,
    read: fn(Answer) -> Read<T>,
    shape: Shaped<E>,
}

impl<'a, T, E> Pending<'a, T, E> {
    /// The call that sends `request`, once written, and reads its answer with `read`, an error's with `shape`.
    pub fn new(
        request: Result<Request<'a>>,
        read: fn(Answer) -> Read<T>,
        shape: Shaped<E>,
    ) -> Self {
        Self {
            request,
            read,
            shape,
        }
    }

    /// Sends the call, and reads its answer as the route's result when its status is a success.
    async fn send(self) -> Result<T, E> {
        let answer = self.request.map_err(Error::cast)?.send(self.shape).await?;

        (self.read)(answer).await.map_err(Error::cast)
    }
}

impl<T, E> fmt::Debug for Pending<'_, T, E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Pending")
            .field("request", &self.request)
            .finish_non_exhaustive()
    }
}

impl<'a, T: Send + 'static, E: Send + 'static> IntoFuture for Pending<'a, T, E> {
    type Output = Result<T, E>;
    type IntoFuture = Pin<Box<dyn Future<Output = Result<T, E>> + Send + 'a>>;

    fn into_future(self) -> Self::IntoFuture {
        Box::pin(self.send())
    }
}

impl<T: Send + 'static, E: Send + 'static> rest::Call<T, E> for Pending<'_, T, E> {
    async fn response(self) -> Result<rest::Response<Result<T, E>>, E> {
        let Self {
            request,
            read,
            shape,
        } = self;
        let request = request.map_err(Error::cast)?;
        let route = request.route;
        let response = request.dispatch().await.map_err(Error::cast)?;
        let status = response.status();
        let version = response.version();
        let headers = response.headers().clone();

        let body = if status.is_success() {
            match read(Answer { response, route }).await {
                Ok(value) => Ok(value),
                // An answer of no content that is not the route's result keeps its status and its headers.
                Err(error) if rest::no_content(status) => Err(error.cast()),
                Err(error) => return Err(error.cast()),
            }
        } else {
            Err(answered_error(response, route, shape).await?)
        };

        Ok(rest::Response::from_parts(status, version, headers, body))
    }

    fn timeout(mut self, duration: Duration) -> Self {
        if let Ok(request) = &mut self.request {
            request.timeout = Some(duration);
        }

        self
    }
}

/// The error of a call that got no answer to read: a handler that returns it answers 502. The URL of a reqwest error
/// it comes from keeps its origin and its path alone, so that a log of the error shows no key of a query scheme.
fn no_answer(
    route: &Route,
    what: &str,
    error: impl Into<Box<dyn StdError + Send + Sync>>,
) -> Error {
    let mut error = error.into();

    redact::strip_urls(&mut *error);
    Error::failed(
        format!("the call `{}` {what}", route.operation_id),
        Some(error),
        StatusCode::BAD_GATEWAY,
    )
}

/// An answer with a success status, read as the route's result.
pub struct Answer {
    response: reqwest::Response,
    route: &'static Route,
}

/// Shows the route, the status and the URL of the answer by its origin and its path, without its query, which may
/// hold a key: reqwest's `Debug` of a response shows the whole URL.
impl fmt::Debug for Answer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Answer")
            .field("operation_id", &self.route.operation_id)
            .field("status", &self.response.status())
            .field("url", &redacted(self.response.url()))
            .finish()
    }
}

impl Answer {
    /// The body, read as the JSON of `T`: what a route returning `T`, `Created<T>` or `Accepted<T>` gives. It is read
    /// whole: it is the route's data. A 204 or a 205, which has no body, is read as `null` when `nullable` says `T`
    /// is `null` when empty, an `Option<T>` or `()`, as its type tells the generated code.
    ///
    /// # Errors
    ///
    /// The read resolves to an error with no status when the body cannot be read, or is not the JSON of `T`, as a 204
    /// or a 205 of a route whose `T` is neither an `Option` nor `()`.
    pub fn json<T: Deserialized + Send + 'static>(self, nullable: bool) -> Read<T> {
        Read::new(self.read_json(nullable))
    }

    /// The body, read as JSON, as [`Answer::json`] reads it.
    async fn read_json<T: DeserializeOwned>(self, nullable: bool) -> Result<T> {
        let route = self.route;
        let empty = nullable && rest::no_content(self.response.status());
        let body = self.read().await?;
        let body: &[u8] = if empty { b"null" } else { &body };

        serde_json::from_slice(body).map_err(|error| no_answer(route, NOT_ITS_RESULT, error))
    }

    /// The body, read whole as UTF-8 text: what a route returning `Text` gives.
    ///
    /// # Errors
    ///
    /// The read resolves to an error with no status when the answer is not `text/plain`, names a charset other than
    /// UTF-8, or its body cannot be read or is not UTF-8.
    pub fn text<T: IsText + Send + 'static>(self) -> Read<T> {
        Read::new(self.read_text())
    }

    /// The body, read as text, as [`Answer::text`] reads it.
    async fn read_text<T: IsText>(self) -> Result<T> {
        let route = self.route;
        let body = self.read_as(TEXT, true).await?;
        // The source is the `Utf8Error` alone: a `FromUtf8Error` would keep the whole body in the error.
        let text = String::from_utf8(body.into())
            .map_err(|error| no_answer(route, NOT_ITS_RESULT, error.utf8_error()))?;

        Ok(T::from_string(text))
    }

    /// The body, read whole as its bytes, of any content type: what a route returning `Bytes` gives. They keep the
    /// answer's content type, or `application/octet-stream`, bytes of no known type, when it has none, or one that is
    /// not a media type, and for a 204 or a 205, which are no bytes.
    ///
    /// # Errors
    ///
    /// The read resolves to an error with no status when the body cannot be read.
    pub fn bytes<B: IsBytes + Send + 'static>(self) -> Read<B> {
        Read::new(self.read_bytes())
    }

    /// The body, read as bytes, as [`Answer::bytes`] reads it.
    async fn read_bytes<B: IsBytes>(self) -> Result<B> {
        let content_type = self.content_type();
        let body = self.read().await?;

        Ok(B::from_parts(body.into(), content_type))
    }

    /// The body, read as its chunks as they come, of any content type: what a route returning `ByteStream` gives. It
    /// keeps the answer's content type, or `application/octet-stream`, as [`Answer::bytes`] does, and ends at once on a
    /// 204 or a 205.
    ///
    /// # Errors
    ///
    /// None to the read itself; in the stream, an error with no status for a body that cannot be read, after which the
    /// stream ends.
    pub fn byte_stream<B: IsByteStream + Send + 'static>(self) -> Read<B> {
        Read::new(self.read_byte_stream())
    }

    /// The body, read as its chunks, as [`Answer::byte_stream`] reads it.
    async fn read_byte_stream<B: IsByteStream>(self) -> Result<B> {
        let content_type = self.content_type();

        Ok(B::from_parts(
            stream::chunks(self.response, self.route),
            content_type,
        ))
    }

    /// The answer's content type, or `application/octet-stream` when it has none, or one that is not a media type, or
    /// when it is a 204 or a 205, whose body, dropped, the type was of.
    fn content_type(&self) -> Cow<'static, str> {
        if rest::no_content(self.response.status()) {
            return Cow::Borrowed(OCTET_STREAM);
        }

        content_type::media_type(self.response.headers())
            .map_or(Cow::Borrowed(OCTET_STREAM), |media_type| {
                Cow::Owned(media_type.to_owned())
            })
    }

    /// The body, read as server-sent events as they come, each the JSON of an item: what a route returning
    /// `Events<T>` gives. Each event that has data is an item, its `data`, whatever its name; an event with no data, a
    /// comment, and the fields `id`, `retry` and any other are skipped.
    ///
    /// # Errors
    ///
    /// The read resolves to an error with no status when the answer is not `text/event-stream`. Then, in the stream,
    /// an error with no status for an event that does not read as an item; the error of an answer for an
    /// `event: problem`, with the problem's status and the problem; and an error with no status for an event or a line
    /// longer than 1 MiB, or a body that cannot be read. The stream ends after any of them but the first.
    pub fn events<E>(self) -> Read<E>
    where
        E: IsEvents + Send + 'static,
        E::Item: Deserialized + Send + 'static,
    {
        Read::new(self.read_events())
    }

    /// The body, read as server-sent events, as [`Answer::events`] reads it.
    async fn read_events<E>(self) -> Result<E>
    where
        E: IsEvents,
        E::Item: DeserializeOwned + Send + 'static,
    {
        let route = self.route;
        let response = self.stream_of(&[EVENT_STREAM])?;

        Ok(E::from_items(stream::items(
            response,
            route,
            stream::EventStream::default(),
        )))
    }

    /// The body, read as JSON Lines as they come, each the JSON of an item: what a route returning `JsonLines<T>`
    /// gives. An empty line is skipped, and a last line with no line break is read too.
    ///
    /// # Errors
    ///
    /// The read resolves to an error with no status when the answer is not `application/jsonl`, `application/x-ndjson`,
    /// `application/x-jsonlines` or `application/json`. Then, in the stream, an error with no status for a line that does not read as an
    /// item; and for a line longer than 1 MiB, or a body that cannot be read, after which the stream ends.
    pub fn json_lines<L>(self) -> Read<L>
    where
        L: IsJsonLines + Send + 'static,
        L::Item: Deserialized + Send + 'static,
    {
        Read::new(self.read_json_lines())
    }

    /// The body, read as JSON Lines, as [`Answer::json_lines`] reads it.
    async fn read_json_lines<L>(self) -> Result<L>
    where
        L: IsJsonLines,
        L::Item: DeserializeOwned + Send + 'static,
    {
        let route = self.route;
        let response = self.stream_of(JSON_LINES_READ)?;

        Ok(L::from_items(stream::items(
            response,
            route,
            stream::JsonLineStream::default(),
        )))
    }

    /// The whole body, empty on a 204 or a 205, or when the answer's content type is `expected`, with no charset but
    /// UTF-8 when `utf8` is set: a
    /// body of another type is not the route's result, even when it would read as one, as a page of HTML would as
    /// text. Such a body is read and dropped up to 64 KiB, as [`Answer::none`] does, so that the connection serves
    /// again.
    async fn read_as(self, expected: &str, utf8: bool) -> Result<bytes::Bytes> {
        // A 204 or a 205 has no body, and so no content type: an empty text.
        if rest::no_content(self.response.status())
            || content_type::is(self.response.headers(), expected, utf8)
        {
            return self.read().await;
        }

        // The call has failed already: a body that does not read only costs the connection.
        let _ = read_up_to_limit(self.response).await;

        Err(not_of_type(self.route, &[expected], utf8))
    }

    /// The answer of a stream, when its content type is one of `expected`, or a 204 or a 205, whose stream ends at
    /// once. An answer of another type is dropped at
    /// once, which closes its connection, rather than drained as [`Answer::read_as`] drains it: it may be an endless
    /// stream of its own, whose 64 KiB never come.
    fn stream_of(self, expected: &[&str]) -> Result<reqwest::Response> {
        let Self { response, route } = self;

        // A 204 or a 205 has no body, and so no content type: a stream that ends at once.
        if rest::no_content(response.status())
            || expected
                .iter()
                .any(|expected| content_type::is(response.headers(), expected, false))
        {
            return Ok(response);
        }

        drop(response);
        Err(not_of_type(route, expected, false))
    }

    /// The whole body, none for a 204 or a 205, whose body, a 205's that a server sends, is read and dropped up to 64
    /// KiB, as [`Answer::none`] does.
    async fn read(self) -> Result<bytes::Bytes> {
        let Self { response, route } = self;

        if rest::no_content(response.status()) {
            // The call has its answer: a body past the limit only costs the connection.
            let _ = read_up_to_limit(response).await;

            return Ok(bytes::Bytes::new());
        }

        response
            .bytes()
            .await
            .map_err(|error| no_answer(route, "got an answer whose body cannot be read", error))
    }

    /// Nothing: what a route with no result gives, whatever the body. A body is read and dropped, up to 64 KiB, so
    /// that the connection serves the next call.
    ///
    /// # Errors
    ///
    /// None: a success status is all such a route answers.
    pub fn none(self) -> Read<()> {
        Read::new(async move {
            // A body past the limit, or one that fails, only costs the connection: the call itself has succeeded.
            let _ = read_up_to_limit(self.response).await;

            Ok(())
        })
    }
}

/// What the error of an answer that is not the route's result says.
const NOT_ITS_RESULT: &str = "got an answer that is not its result";

/// The error of an answer whose content type is none of `expected`, with no charset but UTF-8 when `utf8` is set.
fn not_of_type(route: &Route, expected: &[&str], utf8: bool) -> Error {
    let names: Vec<String> = expected
        .iter()
        .map(|expected| format!("`{expected}`"))
        .collect();
    let names = match names.split_last() {
        Some((last, [])) => last.clone(),
        Some((last, rest)) => format!("{} or {last}", rest.join(", ")),
        None => String::new(),
    };
    let charset = if utf8 { " in UTF-8" } else { "" };

    no_answer(
        route,
        NOT_ITS_RESULT,
        format!("its content type is not {names}{charset}"),
    )
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
    content_type::is(headers, PROBLEM, false)
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
    use http::Method;
    use reqwest::ResponseBuilderExt;

    static ROUTE: Route = Route {
        operation_id: "notes_get",
        group: "Notes",
        method: Method::GET,
        path: "/api/notes/{id}",
        schemes: &[],
        scheme_types: &[],
        streams: false,
    };

    fn client(url: &str) -> Result<Client> {
        Client::new(url)
    }

    fn segment<T: Serialize + ?Sized>(value: &T) -> Result<String> {
        let client = client("http://localhost").expect("a valid url");

        Request::new(&client, &ROUTE)
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
        assert_eq!(
            segment(&u128::MAX).expect("a u128"),
            format!("/{}", u128::MAX)
        );
        assert_eq!(
            segment(&i128::MIN).expect("an i128"),
            format!("/{}", i128::MIN)
        );
        assert_eq!(segment(&1.5_f64).expect("a float"), "/1.5");
        assert_eq!(segment(&'é').expect("a char"), "/%C3%A9");
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
        assert!(
            segment(&None::<i64>)
                .expect_err("no segment")
                .to_string()
                .contains("`id` of `notes_get` is `None`")
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
            Request::new(&client, &ROUTE)
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
    fn an_error_about_a_url_shows_neither_its_credentials_nor_its_query() {
        for url in [
            "http://ana:secret@local host/",
            "ftp://ana:secret@localhost/",
            "http://ana:secret@localhost/?page=1",
            "http://localhost/?token=secret",
            "http://local host/?token=secret",
        ] {
            let error = client(url).expect_err("not the url of an api");
            let mut shown = format!("{error} {error:?}");
            let mut source = error.source();

            while let Some(error) = source {
                shown.push_str(&format!(" {error} {error:?}"));
                source = error.source();
            }

            assert!(
                !shown.contains("secret") && !shown.contains("ana"),
                "{shown}"
            );
        }
    }

    #[test]
    fn an_answer_shows_its_url_without_its_query() {
        let url = Url::parse("http://localhost:8080/api/notes/1?token=secret").expect("a url");
        let response = http::Response::builder()
            .url(url)
            .body("")
            .expect("a response");
        let answer = Answer {
            response: response.into(),
            route: &ROUTE,
        };

        assert_eq!(
            format!("{answer:?}"),
            r#"Answer { operation_id: "notes_get", status: 200, url: "http://localhost:8080/api/notes/1" }"#
        );
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

    /// A shape of another API's whose `Display` is its `message`.
    #[derive(Debug, serde::Deserialize)]
    struct Said {
        message: String,
    }

    impl fmt::Display for Said {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str(&self.message)
        }
    }

    #[test]
    fn a_shape_that_says_nothing_has_no_message() {
        let said = |message: &str| Said {
            message: message.to_owned(),
        };
        let problem = |body: &[u8]| read_problem(body).expect("an object");

        assert_eq!(
            said("Bad credentials").message().as_deref(),
            Some("Bad credentials")
        );
        assert_eq!(said("").message(), None);
        assert_eq!(
            problem(br#"{"detail": "gone"}"#).message().as_deref(),
            Some("gone")
        );
        assert_eq!(problem(br#"{"detail": ""}"#).message(), None);
        assert_eq!(
            Error::answered(StatusCode::NOT_FOUND, said("")).to_string(),
            "another api answered 404"
        );
    }
}
