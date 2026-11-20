//! `compression()`: answers compressed as the caller accepts, streams aside.

use std::convert::Infallible;
use std::fmt;
use std::task::{Context, Poll};

use axum::body::Body;
use axum::extract::Request;
use axum::response::Response;
use tower_http::compression::predicate::{And, DefaultPredicate, NotForContentType, Predicate};
use tower_http::compression::{Compression, CompressionLayer as Compressed};
use tower_layer::Layer;
use tower_service::Service;

use super::layer::ResponseFuture;
use crate::__private::content_type::JSON_LINES;

/// What is compressed: tower-http's default, which leaves out answers of 32 bytes or less, images, gRPC and
/// server-sent events, and JSON Lines too, whose lines would wait in the encoder rather than leave as they come.
type Compressible = And<DefaultPredicate, NotForContentType>;

/// Compresses each answer as the caller's `Accept-Encoding` asks, with gzip, brotli or zstd, on tower-http's
/// `Compression`. Answers of 32 bytes or less, images, and streams, `Events` and `JsonLines`, whose values must
/// leave as they come, are sent as they are.
///
/// ```
/// use quoll::server;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// let router = server::builder()
///     .with(server::compression())
///     .into_router(api_router());
/// ```
pub fn compression() -> CompressionLayer {
    let compressible = DefaultPredicate::new().and(NotForContentType::const_new(JSON_LINES));

    CompressionLayer(
        Compressed::new()
            .gzip(true)
            .br(true)
            .zstd(true)
            .compress_when(compressible),
    )
}

/// The layer [`compression`] gives.
#[derive(Clone)]
pub struct CompressionLayer(Compressed<Compressible>);

impl fmt::Debug for CompressionLayer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CompressionLayer").finish_non_exhaustive()
    }
}

impl<S> Layer<S> for CompressionLayer {
    type Service = CompressionService<S>;

    fn layer(&self, inner: S) -> Self::Service {
        CompressionService(self.0.layer(inner))
    }
}

/// The service of [`CompressionLayer`].
#[derive(Clone)]
pub struct CompressionService<S>(Compression<S, Compressible>);

impl<S> fmt::Debug for CompressionService<S> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CompressionService").finish_non_exhaustive()
    }
}

impl<S> Service<Request> for CompressionService<S>
where
    S: Service<Request, Response = Response, Error = Infallible> + Send + 'static,
    S::Future: Send + 'static,
{
    type Response = Response;
    type Error = Infallible;
    type Future = ResponseFuture;

    fn poll_ready(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), Infallible>> {
        self.0.poll_ready(cx)
    }

    fn call(&mut self, request: Request) -> ResponseFuture {
        let answer = self.0.call(request);

        Box::pin(async move { Ok(answer.await?.map(Body::new)) })
    }
}
