//! The server of a contract: [`builder()`], which serves the router `Api::router` gives until it is told to stop,
//! and the usual tower layers.
//!
//! | Layer             | What it does                                                                             |
//! | ----------------- | ---------------------------------------------------------------------------------------- |
//! | [`request_id()`]  | keeps the caller's `X-Request-Id` or makes one, which a handler reads with [`RequestId`] |
//! | [`trace()`]       | a tracing span per request, and a line per answer                                        |
//! | [`cors()`]        | CORS for the pages of the origins it is given                                            |
//! | [`compression()`] | answers compressed as the caller accepts, streams aside                                  |
//! | [`timeout()`]     | a deadline on each route, answered 503                                                   |
//! | [`body_limit()`]  | the largest body a route reads, answered 413 past it                                     |
//!
//! Each answers an RFC 9457 problem, as handlers do, where tower-http's answer plain text. Each returns a type of
//! [`layer`], which an application does not name. Any other tower layer applies as is.
//!
//! The [`Builder`] puts the layers of quoll in the order they work in, whatever the order they are given in, and any
//! other tower layer where it is given, the first given the outermost:
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
//! added from the innermost to the outermost, `body_limit`, `timeout`, `compression`, `cors`, `trace`, then
//! `request_id`. In another order each layer still does its work, but:
//!
//! - with `trace` outside `request_id`, the span gets the id from `request_id` when it runs, and nothing is lost;
//! - an answer a layer outside `request_id` makes, a 503 of `timeout` or a 413 of `body_limit`, carries no id;
//! - with `timeout` outside `trace`, a request past its deadline has its span dropped with the route's future: its
//!   503 is not traced, and the line `timeout` logs has none of the span's fields;
//! - an early 413 of `body_limit` outside `trace` is not traced;
//! - an answer a layer outside `cors` makes carries no header of CORS, and a page of another origin cannot read it.
//!
//! The answer of each route carries its [`Route`](crate::Route), which a layer of the application's reads, to
//! count the answers of each operation.
//!
//! [`Builder::page`] serves the page a bundler builds beside the api, on the same origin.

pub(crate) mod body_limit;
mod builder;
mod compression;
mod cors;
mod page;
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
