//! `trace()`: a tracing span per request, which names its operation and its id, and a line per answer.

use std::convert::Infallible;
use std::task::{Context, Poll};
use std::time::Duration;

use axum::body::Body;
use axum::extract::Request;
use axum::response::Response;
use http::Extensions;
use tower_http::classify::{ServerErrorsAsFailures, SharedClassifier};
use tower_http::trace::{MakeSpan, OnResponse, Trace};
use tower_layer::Layer;
use tower_service::Service;
use tracing::Span;
use tracing::field::{Empty, display};

use super::RequestId;
use super::layer::ResponseFuture;
use crate::__private::server::Logged;
use crate::rest::Route;

/// The span of a request, in its extensions, where the router names its operation and `request_id` its id once they
/// are known: both come after the span is made, as axum runs a route's layers before the route.
#[derive(Clone)]
struct TraceSpan(Span);

/// Names the operation of `route` in the span of the request whose `extensions` these are, when it has one.
pub(crate) fn record_route(extensions: &Extensions, route: &Route) {
    if let Some(TraceSpan(span)) = extensions.get() {
        span.record("operation", route.operation_id());
    }
}

/// Gives `id` to the span of the request whose `extensions` these are, when it has one.
pub(crate) fn record_request_id(extensions: &Extensions, id: &RequestId) {
    if let Some(TraceSpan(span)) = extensions.get() {
        span.record("request_id", display(id));
    }
}

/// A tracing span per request, and a line per answer.
///
/// It is built on tower-http's `Trace`. The span is `request`, at the level `INFO`, with the request's `method`, its
/// `path` without the query, which may hold a credential, the `operation` of its route, `notes_get`, and its
/// `request_id` when [`request_id`] gives it one. What a handler logs, and what the crate logs of an error, an item of a stream that
/// fails included, is logged in it.
///
/// Each answer is logged once, `answered` with its `status` and its `latency`: at `INFO`, and at `ERROR` for a 5xx
/// the crate has not logged already. An `Error::internal`, an error of another API, a [`timeout`] are logged by the
/// crate with their cause, and their answer at `INFO`; an `Error::new` with a 5xx status, which a handler gave, at
/// `ERROR`.
///
/// ```
/// use quoll::server;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// let router = server::builder()
///     .with(server::request_id())
///     .with(server::trace())
///     .into_router(api_router());
/// ```
///
/// [`request_id`]: super::request_id()
/// [`timeout`]: super::timeout()
pub fn trace() -> TraceLayer {
    TraceLayer(())
}

/// The layer [`trace`] gives.
#[derive(Debug, Clone)]
pub struct TraceLayer(());

impl<S> Layer<S> for TraceLayer {
    type Service = TraceService<S>;

    fn layer(&self, inner: S) -> Self::Service {
        let inner = tower_http::trace::TraceLayer::new_for_http()
            .make_span_with(SpanOf)
            .on_request(())
            .on_response(Answered)
            .on_body_chunk(())
            .on_eos(())
            .on_failure(())
            .layer(inner);

        TraceService { inner }
    }
}

/// tower-http's `Trace`, as [`TraceLayer`] sets it.
type Traced<S> =
    Trace<S, SharedClassifier<ServerErrorsAsFailures>, SpanOf, (), Answered, (), (), ()>;

/// The service of [`TraceLayer`].
#[derive(Debug, Clone)]
pub struct TraceService<S> {
    inner: Traced<S>,
}

impl<S> Service<Request> for TraceService<S>
where
    S: Service<Request, Response = Response, Error = Infallible> + Send + 'static,
    S::Future: Send + 'static,
{
    type Response = Response;
    type Error = Infallible;
    type Future = ResponseFuture;

    fn poll_ready(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), Infallible>> {
        self.inner.poll_ready(cx)
    }

    fn call(&mut self, mut request: Request) -> ResponseFuture {
        let span = tracing::info_span!(
            "request",
            method = %request.method(),
            path = request.uri().path(),
            operation = Empty,
            request_id = Empty,
        );

        if let Some(id) = request.extensions().get::<RequestId>() {
            span.record("request_id", display(id));
        }

        request.extensions_mut().insert(TraceSpan(span));

        let answer = self.inner.call(request);

        Box::pin(async move { Ok(answer.await?.map(Body::new)) })
    }
}

/// The span `Trace` runs a request in: the one [`TraceService`] made, which the request carries.
#[derive(Debug, Clone, Copy)]
struct SpanOf;

impl<B> MakeSpan<B> for SpanOf {
    fn make_span(&mut self, request: &http::Request<B>) -> Span {
        request
            .extensions()
            .get::<TraceSpan>()
            .map_or_else(Span::none, |TraceSpan(span)| span.clone())
    }
}

/// Logs each answer once.
#[derive(Debug, Clone, Copy)]
struct Answered;

impl<B> OnResponse<B> for Answered {
    fn on_response(self, response: &http::Response<B>, latency: Duration, _span: &Span) {
        let status = response.status();

        if status.is_server_error() && response.extensions().get::<Logged>().is_none() {
            tracing::error!(status = status.as_u16(), ?latency, "answered");
            return;
        }

        tracing::info!(status = status.as_u16(), ?latency, "answered");
    }
}
