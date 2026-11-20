//! `timeout(duration)`: a deadline on each handler, answered 503.

use std::convert::Infallible;
use std::task::{Context, Poll};
use std::time::Duration;

use axum::extract::Request;
use axum::response::{IntoResponse, Response};
use http::StatusCode;
use tower_layer::Layer;
use tower_service::Service;

use super::layer::ResponseFuture;
use crate::__private::server::Logged;
use crate::rest::Error;

/// A deadline on each request: a route that has not answered within `duration` answers 503, with a problem whose
/// detail says the deadline, and the crate logs it once, at `ERROR`.
///
/// It bounds the route's future, which reads the body, calls the handler and ends with the head of its answer, and
/// not the answer's body: an `Events`, a `JsonLines` or a `ByteStream`, whose handler returns as soon as it has the
/// stream, goes on past it. The handler's future is dropped at the deadline, so that its work stops at its next `.await`.
///
/// ```
/// use std::time::Duration;
///
/// use quoll::server;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// let router = server::builder()
///     .with(server::timeout(Duration::from_secs(10)))
///     .into_router(api_router());
/// ```
pub fn timeout(duration: Duration) -> TimeoutLayer {
    TimeoutLayer { duration }
}

/// The layer [`timeout`] gives.
#[derive(Debug, Clone, Copy)]
pub struct TimeoutLayer {
    duration: Duration,
}

impl<S> Layer<S> for TimeoutLayer {
    type Service = TimeoutService<S>;

    fn layer(&self, inner: S) -> Self::Service {
        TimeoutService {
            inner,
            duration: self.duration,
        }
    }
}

/// The service of [`TimeoutLayer`].
#[derive(Debug, Clone)]
pub struct TimeoutService<S> {
    inner: S,
    duration: Duration,
}

impl<S> Service<Request> for TimeoutService<S>
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

    fn call(&mut self, request: Request) -> ResponseFuture {
        let duration = self.duration;
        let answer = self.inner.call(request);

        Box::pin(async move {
            let Ok(answer) = tokio::time::timeout(duration, answer).await else {
                tracing::error!(
                    ?duration,
                    "the route took longer than its deadline, answered 503"
                );

                let detail = format!("the server took longer than {duration:?} to answer");
                let mut response =
                    Error::new(StatusCode::SERVICE_UNAVAILABLE, detail).into_response();

                response.extensions_mut().insert(Logged);
                return Ok(response);
            };

            Ok(answer?.into_response())
        })
    }
}
