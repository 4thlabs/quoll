//! The server a contract becomes: a [`Builder`] that serves the router `Api::router` gives, until it is told to stop,
//! and the usual tower layers, each answering RFC 9457 problems as handlers do, where tower-http's answer plain text.
//!
//! The builder puts the layers of quoll in the order they work in, whatever the order they are given in, and any other
//! tower layer where it is given, the first given the outermost:
//!
//! ```no_run
//! use std::time::Duration;
//!
//! use quoll::server;
//!
//! # fn api_router() -> axum::Router { axum::Router::new() }
//! # async fn run() -> std::io::Result<()> {
//! server::builder()
//!     .with(server::request_id())
//!     .with(server::trace())
//!     .with(server::timeout(Duration::from_secs(10)))
//!     .with(server::body_limit(1 << 20))
//!     .serve("0.0.0.0:8080", api_router())
//!     .await?;
//! # Ok(())
//! # }
//! ```
//!
//! So a request gets its id first, then its span, which carries the id, then its deadline and the limit of its body,
//! and every answer, a 503 of the deadline or a 413 of the limit too, is traced and carries the id.
//!
//! Each helper gives a layer for `Router::layer` too, which runs the last layer added first: there, the layers are
//! added from the innermost to the outermost, `body_limit`, `timeout`, `trace`, then `request_id`. In another order
//! each layer still does its work, but:
//!
//! - with `trace` outside `request_id`, the span gets the id from `request_id` when it runs, and nothing is lost;
//! - an answer a layer outside `request_id` makes, a 503 of `timeout` or a 413 of `body_limit`, carries no id;
//! - with `timeout` outside `trace`, a request past its deadline has its span dropped with the route's future: its
//!   503 is not traced, and the line `timeout` logs has none of the span's fields;
//! - an early 413 of `body_limit` outside `trace` is not traced;
//! - an answer a layer inside `cors` makes carries no header of CORS, and a page of another origin cannot read it.
//!
//! The router puts the [`Route`](crate::rest::Route) a request called in the extensions of its answer, where a layer
//! of the application's reads it, to count the answers of each operation. An answer no route made carries none: axum's
//! 404 and 405, and the answers of the layers, a 503 of `timeout` and an early 413 of `body_limit`, which come before
//! the route or instead of it. Any other tower layer applies as is.

pub(crate) mod body_limit;
mod builder;
mod compression;
mod cors;
mod request_id;
mod timeout;
pub(crate) mod trace;

pub use body_limit::body_limit;
pub use builder::{Address, Builder, builder};
pub use compression::compression;
pub use cors::{InvalidOrigin, cors};
pub use request_id::{RequestId, request_id};
pub use timeout::timeout;
pub use trace::trace;

/// The layers the helpers of [`server`](self) give, and the services they wrap a router's routes in: an
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
