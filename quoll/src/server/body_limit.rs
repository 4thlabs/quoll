//! `body_limit(bytes)`: the largest body a route reads, answered 413 past it.

use std::convert::Infallible;
use std::task::{Context, Poll};

use axum::extract::{DefaultBodyLimit, Request};
use axum::response::{IntoResponse, Response};
use http::{HeaderMap, StatusCode, header};
use tower_layer::Layer;
use tower_service::Service;

use super::layer::ResponseFuture;
use crate::Error;

/// The 413 of a body larger than `limit`, the one enforced, or than a limit the router cannot name: axum's, which
/// `body_limit` sets or a route may set lower.
pub(crate) fn too_large(limit: Option<usize>) -> Error {
    let detail = match limit {
        Some(limit) => format!("the body is larger than the {limit} bytes this server reads"),
        None => "the body is larger than this server reads".to_owned(),
    };

    Error::new(StatusCode::PAYLOAD_TOO_LARGE, detail)
}

/// The largest body a route reads, in bytes: a larger one answers 413, with a problem.
///
/// A request whose `Content-Length` is larger is answered at once, before its body is read, with the limit in the
/// problem's detail; the connection is closed after the answer, so that a client still sending the body has its write
/// refused, the 413 waiting to be read. One with no length, a chunked body, is read up to the limit, then answered so,
/// whatever the route reads: JSON, a `Form`, `Text` or `Bytes`. That limit is axum's own, `DefaultBodyLimit`,
/// which this layer sets, replacing its default of 2 MiB, and which a route may set lower: the detail of its 413 says
/// no number then.
///
/// ```
/// use quoll::server;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// let router = server::builder()
///     .with(server::body_limit(1 << 20))
///     .into_router(api_router());
/// ```
pub fn body_limit(bytes: usize) -> BodyLimitLayer {
    BodyLimitLayer { limit: bytes }
}

/// The layer [`body_limit`] gives.
#[derive(Debug, Clone, Copy)]
pub struct BodyLimitLayer {
    limit: usize,
}

impl<S> Layer<S> for BodyLimitLayer {
    type Service = BodyLimitService<S>;

    fn layer(&self, inner: S) -> Self::Service {
        BodyLimitService {
            inner,
            limit: self.limit,
        }
    }
}

/// The service of [`BodyLimitLayer`].
#[derive(Debug, Clone)]
pub struct BodyLimitService<S> {
    inner: S,
    limit: usize,
}

impl<S> Service<Request> for BodyLimitService<S>
where
    S: Service<Request, Error = Infallible> + Send + 'static,
    S::Response: IntoResponse,
    S::Future: Send + 'static,
{
    type Response = Response;
    type Error = Infallible;
    type Future = ResponseFuture;

    fn poll_ready(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), Infallible>> {
        self.inner.poll_ready(cx)
    }

    fn call(&mut self, mut request: Request) -> ResponseFuture {
        if content_length(request.headers()).is_some_and(|length| length > self.limit as u64) {
            let response = too_large(Some(self.limit)).into_response();

            return Box::pin(async move { Ok(response) });
        }

        // axum's extractors, through which the router reads every body, stop at this limit.
        DefaultBodyLimit::max(self.limit).apply(&mut request);

        let answer = self.inner.call(request);

        Box::pin(async move { Ok(answer.await?.into_response()) })
    }
}

/// The request's `Content-Length`, when it gives one that reads; hyper has refused a request whose length does not.
fn content_length(headers: &HeaderMap) -> Option<u64> {
    headers
        .get(header::CONTENT_LENGTH)?
        .to_str()
        .ok()?
        .parse()
        .ok()
}
