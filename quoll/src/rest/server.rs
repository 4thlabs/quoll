//! Tower layers for the server a contract becomes: the usual ones, each answering RFC 9457 problems as handlers do,
//! where tower-http's answer plain text.
//!
//! Each helper gives a layer for `Router::layer`, on the router `Api::router` gives. axum runs the last layer added
//! first, so the layers are added from the innermost to the outermost:
//!
//! ```
//! use std::time::Duration;
//!
//! use quoll::rest;
//!
//! # fn api_router() -> axum::Router { axum::Router::new() }
//! fn router() -> axum::Router {
//!     api_router()
//!         .layer(rest::server::body_limit(1 << 20))
//!         .layer(rest::server::timeout(Duration::from_secs(10)))
//!         .layer(rest::server::trace())
//!         .layer(rest::server::request_id())
//! }
//! # router();
//! ```
//!
//! So a request gets its id first, then its span, which carries the id, then its deadline and the limit of its body,
//! and every answer, a 503 of the deadline or a 413 of the limit too, is traced and carries the id. Each layer works
//! in any order: in another, the span of a request no layer has given an id yet gets it from `request_id` when it
//! does, and an answer of a layer outside `request_id` carries no id.
//!
//! The router puts the [`Route`](crate::rest::Route) a request called in the extensions of its answer, where a layer
//! of the application's reads it, to count the answers of each operation. Any other tower layer applies as is.

pub(crate) mod body_limit;
mod compression;
mod cors;
mod request_id;
mod timeout;
pub(crate) mod trace;

pub use body_limit::body_limit;
pub use compression::compression;
pub use cors::{InvalidOrigin, cors};
pub use request_id::{RequestId, request_id};
pub use timeout::timeout;
pub use trace::trace;

/// The layers the helpers of [`rest::server`](self) give, and the services they wrap a router's routes in: an
/// application gives them to `Router::layer`, and never names them.
pub mod layer {
    use std::convert::Infallible;
    use std::future::Future;
    use std::pin::Pin;

    use axum::response::Response;

    pub use super::body_limit::{BodyLimitLayer, BodyLimitService};
    pub use super::compression::{CompressionLayer, CompressionService};
    pub use super::cors::{CorsLayer, CorsService};
    pub use super::request_id::{RequestIdLayer, RequestIdService};
    pub use super::timeout::{TimeoutLayer, TimeoutService};
    pub use super::trace::{TraceLayer, TraceService};

    /// The answer a service of these layers gives, once its route has answered.
    pub type ResponseFuture = Pin<Box<dyn Future<Output = Result<Response, Infallible>> + Send>>;
}
