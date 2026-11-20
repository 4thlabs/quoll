//! The server side of what `rest::api!` writes: the routes of a group on an axum router, the caller authenticated and
//! the arguments read from a request, and the answers, streams among them, an error answered as an RFC 9457
//! problem.

use std::borrow::Cow;
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

use crate::__private::content_type::{
    self, FORM, JSON, MULTIPART, OCTET_STREAM, PROBLEM, TEXT, TEXT_UTF8, media_type_value,
};
use crate::__private::known::{IsAccepted, IsBytes, IsCreated, IsForm, IsText};
use crate::__private::read::{Deserialized, Read};
use crate::rest::error::Repr;
use crate::rest::{Authenticate, Error, Result, Route, Scheme, Scoped};
use crate::server::{body_limit, trace};

mod stream;
mod text;

pub use axum::routing::MethodFilter;
pub use stream::{Written, events, json_lines};

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

    /// Serves `route` with `handler`, given the service and the request. An error it returns is answered as a
    /// problem; a 401 carries a `WWW-Authenticate` challenge for each scheme of `C`, the schemes the route takes, as
    /// RFC 9110 asks, and none on a route that takes no scheme, `()`. Every answer carries the route in its
    /// extensions, where a layer reads it, and the span of `server::trace` names its operation.
    ///
    /// # Panics
    ///
    /// When axum refuses the route's method or its path, which the macro has checked it would not.
    pub fn route<C, F, Fut>(self, route: &'static Route, handler: F) -> Self
    where
        C: Challenges,
        F: Fn(Arc<S>, Request) -> Fut + Clone + Send + Sync + 'static,
        Fut: Future<Output = Result<Response>> + Send + 'static,
    {
        let method = MethodFilter::try_from(route.method().clone())
            .expect("the macro writes a method axum serves");
        let service = Arc::clone(&self.service);
        let endpoint = on(method, move |request: axum::extract::Request| {
            trace::record_route(request.extensions(), route);

            let answer = handler(Arc::clone(&service), Request::new(request));

            async move {
                let mut response = answer
                    .await
                    .unwrap_or_else(|error| challenged::<C>(error.into_response()));

                response.extensions_mut().insert(route.clone());
                response
            }
        });

        Self {
            router: self.router.route(route.path(), endpoint),
            service: self.service,
        }
    }

    /// The router, with the group's routes.
    pub fn finish(self) -> Router {
        self.router
    }
}

/// The schemes a route takes, whose challenges its 401 carries: `()` for none, a scheme, or a tuple of two to four
/// schemes for `#[auth(A | B)]`, whose challenges come in their order.
pub trait Challenges: 'static {
    /// Adds a `WWW-Authenticate` per scheme to the headers of a 401.
    fn challenge(headers: &mut HeaderMap);
}

impl Challenges for () {
    fn challenge(_headers: &mut HeaderMap) {}
}

impl<S: Scheme> Challenges for S {
    fn challenge(headers: &mut HeaderMap) {
        headers.append(header::WWW_AUTHENTICATE, S::CHALLENGE);
    }
}

/// A service that authenticates the credentials of the schemes `C`: a scheme, or a tuple of two to four schemes for
/// `#[auth(A | B)]`, which give one caller. The first scheme whose credential a request carries decides: a wrong one is
/// refused, and the next schemes are not tried.
pub trait Authenticates<C>: Send + Sync + 'static {
    /// The caller every scheme of `C` gives.
    type Caller: Send + Sync + 'static;

    /// The credential the request carries, of the first scheme that finds one.
    type Found: Send + 'static;

    /// The credential of the first scheme of `C` the request carries; `None` when it carries none.
    ///
    /// # Errors
    ///
    /// The error of the first scheme whose credential is not well formed.
    fn find(request: &Parts) -> Result<Option<Self::Found>>;

    /// The caller `found` stands for, from the service's `authenticate` for its scheme.
    fn authenticate(&self, found: Self::Found)
    -> impl Future<Output = Result<Self::Caller>> + Send;
}

impl<A: Authenticate<S>, S: Scheme> Authenticates<S> for A {
    type Caller = A::Caller;
    type Found = S::Credential;

    fn find(request: &Parts) -> Result<Option<S::Credential>> {
        S::read(request)
    }

    fn authenticate(&self, found: S::Credential) -> impl Future<Output = Result<A::Caller>> + Send {
        Authenticate::<S>::authenticate(self, found)
    }
}

/// The credential of one scheme of an alternative, or of the ones after it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Either<L, R> {
    /// The first scheme's.
    First(L),
    /// One of the next schemes'.
    Next(R),
}

/// Implements `Challenges` and `Authenticates` for a tuple of schemes: its first one, then the tuple of the others.
macro_rules! alternatives {
    ($first:ident, $rest:ty; $($scheme:ident),+) => {
        impl<$($scheme: Scheme),+> Challenges for ($($scheme,)+) {
            fn challenge(headers: &mut HeaderMap) {
                $(<$scheme as Challenges>::challenge(headers);)+
            }
        }

        impl<A, $($scheme: Scheme),+> Authenticates<($($scheme,)+)> for A
        where
            A: Authenticates<$first> + Authenticates<$rest, Caller = <A as Authenticates<$first>>::Caller>,
        {
            type Caller = <A as Authenticates<$first>>::Caller;
            type Found = Either<<A as Authenticates<$first>>::Found, <A as Authenticates<$rest>>::Found>;

            fn find(request: &Parts) -> Result<Option<Self::Found>> {
                if let Some(found) = <A as Authenticates<$first>>::find(request)? {
                    return Ok(Some(Either::First(found)));
                }

                Ok(<A as Authenticates<$rest>>::find(request)?.map(Either::Next))
            }

            fn authenticate(&self, found: Self::Found) -> impl Future<Output = Result<Self::Caller>> + Send {
                async move {
                    match found {
                        Either::First(found) => <A as Authenticates<$first>>::authenticate(self, found).await,
                        Either::Next(found) => <A as Authenticates<$rest>>::authenticate(self, found).await,
                    }
                }
            }
        }
    };
}

alternatives!(S1, S2; S1, S2);
alternatives!(S1, (S2, S3); S1, S2, S3);
alternatives!(S1, (S2, S3, S4); S1, S2, S3, S4);

/// Refuses a caller that lacks one of `scopes`, the scopes of a route's `#[auth(...)]`, every one of which it needs.
///
/// # Errors
///
/// A 403 that names the first scope the caller lacks.
pub fn authorize<C: Scoped + ?Sized>(caller: &C, scopes: &[&str]) -> Result<()> {
    match scopes.iter().find(|scope| !caller.has_scope(scope)) {
        Some(scope) => Err(Error::new(
            StatusCode::FORBIDDEN,
            format!("the caller lacks the scope `{scope}`, which this route takes"),
        )),
        None => Ok(()),
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

    /// The caller `service` makes of the credential of the schemes `C` the request carries: a scheme, or a tuple of
    /// schemes of which the first one whose credential the request carries decides. Only the headers and the query
    /// are read: a request refused never has its body read.
    ///
    /// # Errors
    ///
    /// A 401 when the request carries no credential of `C`, or one that is not well formed; the error of `service`'s
    /// `authenticate` as it is, a 401 for a credential it does not know or a 500 for its own failure.
    pub fn authenticate<'a, C, A>(
        &self,
        service: &'a A,
    ) -> impl Future<Output = Result<A::Caller>> + Send + use<'a, C, A>
    where
        A: Authenticates<C>,
    {
        let caller = self.authenticate_optional::<C, A>(service);

        async move {
            caller.await?.ok_or_else(|| {
                Error::unauthorized("the request carries no credential, which this route takes")
            })
        }
    }

    /// The caller `service` makes of the credential of the schemes `C`, as [`authenticate`](Request::authenticate),
    /// or `None` when the request carries no credential: a route with `#[auth(..., optional)]`.
    ///
    /// # Errors
    ///
    /// A 401 for a credential that is not well formed; the error of `service`'s `authenticate` as it is.
    pub fn authenticate_optional<'a, C, A>(
        &self,
        service: &'a A,
    ) -> impl Future<Output = Result<Option<A::Caller>>> + Send + use<'a, C, A>
    where
        A: Authenticates<C>,
    {
        // Read now, so that the future does not borrow the request, whose body is not `Sync`.
        let found = <A as Authenticates<C>>::find(&self.parts);

        async move {
            match found? {
                Some(found) => <A as Authenticates<C>>::authenticate(service, found)
                    .await
                    .map(Some),
                None => Ok(None),
            }
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

        from_form(query.as_bytes(), "the query")
    }

    /// The header `name`, read from its text as a path parameter is, into `T`: an integer, a string, a unit variant,
    /// or an `Option` of one, which is `None` when the header is missing.
    ///
    /// # Errors
    ///
    /// A 400 when the header is missing and `T` is not an `Option`, given more than once, not UTF-8, or not a `T`.
    pub fn header<T: DeserializeOwned>(&self, name: &'static str) -> Result<T> {
        let mut values = self.parts.headers.get_all(name).iter();
        let value = values.next();

        if values.next().is_some() {
            return Err(Error::bad_request(format!(
                "the header `{name}` is given more than once, and this route reads one value"
            )));
        }

        let text = value
            .map(|value| std::str::from_utf8(value.as_bytes()))
            .transpose()
            .map_err(|_| Error::bad_request(format!("the header `{name}` is not UTF-8")))?;

        text::from_text(text).map_err(|error| match error {
            text::TextError::Missing => Error::bad_request(format!(
                "the request has no header `{name}`, which this route takes"
            )),
            text::TextError::Invalid(message) => {
                Error::bad_request(format!("the header `{name}` is not valid: {message}"))
            }
        })
    }

    /// The body, read as the JSON of `T`.
    ///
    /// # Errors
    ///
    /// A 400 when the body is not the JSON of `T`, a 415 when the request does not say it sends JSON or names more
    /// than one content type, and the status axum gives when the body cannot be read, as a 413 for one too large.
    pub fn json<T: Deserialized + Send + 'static>(self) -> Read<T> {
        Read::new(self.read_json())
    }

    /// The body, read as JSON, as [`Request::json`] reads it.
    async fn read_json<T: DeserializeOwned>(self) -> Result<T> {
        // axum reads the first `Content-Type` alone: a second one is refused here, as every other body refuses it.
        if self
            .parts
            .headers
            .get_all(header::CONTENT_TYPE)
            .iter()
            .nth(1)
            .is_some()
        {
            return Err(unsupported(format!(
                "the body is read as `{JSON}`: send it with one `Content-Type: {JSON}`"
            )));
        }

        let request = http::Request::from_parts(self.parts, self.body);

        match axum::Json::<T>::from_request(request, &()).await {
            Ok(axum::Json(value)) => Ok(value),
            Err(JsonRejection::JsonDataError(error)) => Err(Error::bad_request(error.body_text())),
            Err(JsonRejection::JsonSyntaxError(error)) => {
                Err(Error::bad_request(error.body_text()))
            }
            Err(error) => Err(body_rejected(error.status(), error.body_text())),
        }
    }

    /// The body, read as a form into the `T` of `Form<T>`, as the query is.
    ///
    /// # Errors
    ///
    /// A 415 when the request does not say it sends `application/x-www-form-urlencoded`, or names a charset other than
    /// UTF-8; a 400 when the form does not read as `T` or a value is not UTF-8 once decoded, and the status axum gives
    /// when the body cannot be read.
    pub fn form<F>(self) -> Read<F>
    where
        F: IsForm + Send + 'static,
        F::Value: Deserialized,
    {
        Read::new(self.read_form())
    }

    /// The body, read as a form, as [`Request::form`] reads it.
    async fn read_form<F>(self) -> Result<F>
    where
        F: IsForm,
        F::Value: DeserializeOwned,
    {
        self.check_content_type(FORM, true)?;

        let body = self.read().await?;

        from_form(&body, "the form").map(F::from_value)
    }

    /// The body, read as UTF-8 text.
    ///
    /// # Errors
    ///
    /// A 415 when the request does not say it sends `text/plain`, or names a charset other than UTF-8; a 400 when the
    /// body is not UTF-8, and the status axum gives when the body cannot be read.
    pub fn text<T: IsText + Send + 'static>(self) -> Read<T> {
        Read::new(self.read_text())
    }

    /// The body, read as text, as [`Request::text`] reads it.
    async fn read_text<T: IsText>(self) -> Result<T> {
        self.check_content_type(TEXT, true)?;

        let body = self.read().await?;
        let text = String::from_utf8(body.into())
            .map_err(|_| Error::bad_request("the body is not UTF-8 text"))?;

        Ok(T::from_string(text))
    }

    /// The body, as its bytes, with the content type the request names.
    ///
    /// # Errors
    ///
    /// A 415 when the request names no content type, more than one, one that is not a media type, or one of the
    /// three a browser sends to another site without asking, and the status axum gives when the body cannot be read.
    pub fn bytes<B: IsBytes + Send + 'static>(self) -> Read<B> {
        Read::new(self.read_bytes())
    }

    /// The body, read as bytes, as [`Request::bytes`] reads it.
    async fn read_bytes<B: IsBytes>(self) -> Result<B> {
        let content_type = self.bytes_content_type()?;
        let body = self.read().await?;

        Ok(B::from_parts(body.into(), Cow::Owned(content_type)))
    }

    /// The content type of a body read as bytes, which may be any media type but those a page of another site posts
    /// without asking: a form, a form with files and text. A body without one is refused too, as a page sends bytes
    /// with none, a `fetch` of an `ArrayBuffer` or a `Blob` of no type, without asking either. So a route that reads
    /// bytes, as one that reads JSON, sees only what a page of its own site or a client other than a browser sends.
    fn bytes_content_type(&self) -> Result<String> {
        if let Some(media_type) = content_type::media_type(&self.parts.headers)
            && !content_type::is_cors_safelisted(media_type)
        {
            return Ok(media_type.to_owned());
        }

        Err(unsupported(format!(
            "the body is read as bytes: send it with one `Content-Type`, `{OCTET_STREAM}` when it has no other, but not `{FORM}`, `{MULTIPART}` or `{TEXT}`"
        )))
    }

    /// A 415 unless the request's one content type is `expected`, whatever its parameters, but a charset other than
    /// UTF-8 when `utf8` is set; a request without one, or with more than one, is refused too, as a JSON body is.
    ///
    /// The body is read as its type says, and no other. A form and text are among the types a browser sends to another
    /// site without asking it first (with no CORS preflight), so the check does not keep out what a page of another
    /// site posts: a route whose credential the browser adds on its own, a cookie, needs a defense of its own. A
    /// Bearer token is never added so.
    fn check_content_type(&self, expected: &str, utf8: bool) -> Result<()> {
        if content_type::is(&self.parts.headers, expected, utf8) {
            return Ok(());
        }

        let charset = if utf8 { "; charset=utf-8" } else { "" };

        Err(unsupported(format!(
            "the body is read as `{expected}`: send it with one `Content-Type: {expected}{charset}`"
        )))
    }

    /// The whole body, within the limit axum sets.
    async fn read(self) -> Result<axum::body::Bytes> {
        let request = http::Request::from_parts(self.parts, self.body);

        axum::body::Bytes::from_request(request, &())
            .await
            .map_err(|error| body_rejected(error.status(), error.body_text()))
    }
}

/// A 415, which says what to send instead.
fn unsupported(detail: String) -> Error {
    Error::new(StatusCode::UNSUPPORTED_MEDIA_TYPE, detail)
}

/// `form`, the query or a body, read as serde_html_form reads a form into `T`; `what` names it in the errors.
fn from_form<T: DeserializeOwned>(form: &[u8], what: &str) -> Result<T> {
    // serde_html_form decodes a value that is not UTF-8 with U+FFFD in it, which would reach the handler as text the
    // caller never sent. `&` and `=` are ASCII, so the whole form decodes as UTF-8 when each value does.
    if percent_encoding::percent_decode(form)
        .decode_utf8()
        .is_err()
    {
        return Err(Error::bad_request(format!(
            "{what} is not UTF-8 once decoded"
        )));
    }

    serde_html_form::from_bytes(form)
        .map_err(|error| Error::bad_request(format!("{what} is not valid: {error}")))
}

/// What axum refused in a body: a 413 for one larger than its limit, said as `server::body_limit` says it but
/// with no number, as the limit axum enforced may be a route's own, or else as [`rejected`] says it.
fn body_rejected(status: StatusCode, text: String) -> Error {
    if status == StatusCode::PAYLOAD_TOO_LARGE {
        return body_limit::too_large(None);
    }

    rejected(status, text)
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
pub fn created<C>(result: Result<C>) -> Result<Response>
where
    C: IsCreated,
    C::Value: Serialize,
{
    let (location, value) = result?.into_parts();
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

/// A 202 with the JSON of what the handler accepted.
///
/// # Errors
///
/// The handler's error, or a 500 when its value cannot be written as JSON.
pub fn accepted<A>(result: Result<A>) -> Result<Response>
where
    A: IsAccepted,
    A::Value: Serialize,
{
    json(StatusCode::ACCEPTED, &result?.into_value())
}

/// A 200 with the text the handler returned, `text/plain; charset=utf-8`.
///
/// # Errors
///
/// The handler's error.
pub fn ok_text<T: IsText>(result: Result<T>) -> Result<Response> {
    let content_type = HeaderValue::from_static(TEXT_UTF8);

    Ok(answer_body(
        content_type,
        result?.into_string().into_bytes(),
    ))
}

/// A 200 with the bytes the handler returned, of their content type, and `X-Content-Type-Options: nosniff`: a browser
/// reads them only as that type says, so that bytes typed `application/octet-stream` are never run as a page.
///
/// # Errors
///
/// The handler's error, or a 500 when their content type is not a media type.
pub fn ok_bytes<B: IsBytes>(result: Result<B>) -> Result<Response> {
    let (bytes, content_type) = result?.into_parts();
    let content_type = media_type_value(content_type).map_err(|content_type| {
        Error::internal(format!(
            "the content type of a `Bytes`, {content_type:?}, is not a media type"
        ))
    })?;
    let mut response = answer_body(content_type, bytes);

    response.headers_mut().insert(
        header::X_CONTENT_TYPE_OPTIONS,
        HeaderValue::from_static("nosniff"),
    );

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
    let content_type = [(header::CONTENT_TYPE, HeaderValue::from_static(JSON))];

    Ok((status, content_type, body).into_response())
}

/// A 200 with `body`, of `content_type`.
fn answer_body(content_type: HeaderValue, body: Vec<u8>) -> Response {
    let content_type = [(header::CONTENT_TYPE, content_type)];

    (StatusCode::OK, content_type, body).into_response()
}

/// What the server answers for this error: the status and, when it is to be sent, the detail; nothing is logged.
fn answered(error: &Error) -> (StatusCode, Option<&str>) {
    match &*error.0 {
        Repr::Problem { status, detail }
            if status.is_client_error() || status.is_server_error() =>
        {
            (*status, Some(detail))
        }
        Repr::Problem { .. } | Repr::Internal(_) => (StatusCode::INTERNAL_SERVER_ERROR, None),
        Repr::Upstream { .. } => (StatusCode::BAD_GATEWAY, None),
        Repr::Failed { answered, .. } => (*answered, None),
    }
}

/// The JSON of the RFC 9457 problem the server answers for this error, with its status; nothing is logged.
fn problem_json(error: &Error) -> (StatusCode, Vec<u8>) {
    let (status, detail) = answered(error);
    let problem = Problem {
        kind: "about:blank",
        title: status.canonical_reason(),
        status: status.as_u16(),
        detail,
    };
    let body = serde_json::to_vec(&problem)
        .expect("a problem holds strings and a number, which JSON always writes");

    (status, body)
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
        // Logs what is not sent, which is all `answer` logs.
        let logged = answer(&self).1.is_none();

        let (status, body) = problem_json(&self);
        let content_type = [(header::CONTENT_TYPE, HeaderValue::from_static(PROBLEM))];
        let mut response = (status, content_type, body).into_response();

        if logged {
            response.extensions_mut().insert(Logged);
        }

        response
    }
}

/// Marks an answer whose error the crate has logged already, so that `server::trace` does not log it as an error
/// a second time.
#[derive(Debug, Clone, Copy)]
pub(crate) struct Logged;

/// What `f` logs on this thread, and what it gives.
///
/// One subscriber serves every unit test, set once as the global default, and writes each event into the log of the
/// thread it happens on, if that thread captures one. A subscriber per call, set as the thread's default, would race:
/// tracing caches whether a callsite is enabled across every subscriber, and a test running beside another, with none,
/// may find it cached as disabled and log nothing.
#[cfg(test)]
fn logged_by<R>(f: impl FnOnce() -> R) -> (String, R) {
    use std::cell::RefCell;
    use std::sync::Once;

    thread_local! {
        /// What this thread logs, while it captures it.
        static CAPTURED: RefCell<Option<Vec<u8>>> = const { RefCell::new(None) };
    }

    /// Writes an event into the log its thread captures, or nowhere.
    struct ThreadLog;

    impl std::io::Write for ThreadLog {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            CAPTURED.with_borrow_mut(|captured| {
                if let Some(log) = captured {
                    log.extend_from_slice(bytes);
                }
            });

            Ok(bytes.len())
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    static INSTALLED: Once = Once::new();

    INSTALLED.call_once(|| {
        let subscriber = tracing_subscriber::fmt()
            .with_writer(|| ThreadLog)
            .with_ansi(false)
            .finish();

        tracing::subscriber::set_global_default(subscriber)
            .expect("no other subscriber is set in the unit tests");
    });

    CAPTURED.with_borrow_mut(|captured| *captured = Some(Vec::new()));

    let given = f();
    let log = CAPTURED
        .with_borrow_mut(Option::take)
        .expect("this thread captures its log");

    (String::from_utf8(log).expect("the log is text"), given)
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
        let (log, (status, detail)) = logged_by(|| {
            let (status, detail) = answer(error);

            (status, detail.map(str::to_owned))
        });

        (log, status, detail)
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
        static UPSTREAM: Route = Route {
            operation_id: "upstream",
            group: "Upstream",
            method: http::Method::GET,
            path: "/upstream",
            schemes: &[],
            scheme_types: &[],
            streams: false,
        };

        let router = Group::new(Router::new(), ())
            .route::<(), _, _>(&UPSTREAM, |_, _| async {
                Err(Error::upstream(
                    StatusCode::NOT_FOUND,
                    None,
                    br#"{"detail":"the other api's secret"}"#.to_vec(),
                ))
            })
            .finish();
        let url = crate::test::spawn(router).await;

        let response = reqwest::get(format!("{url}/upstream"))
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
    }
}
