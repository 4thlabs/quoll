//! The builder of a server: the layers of quoll in the order they work in, any other layer where it is given, and the
//! loop that serves a router until it is told to stop.

use std::any::TypeId;
use std::convert::Infallible;
use std::fmt;
use std::future::{Future, IntoFuture, pending};
use std::io;
use std::pin::{Pin, pin};
use std::time::Duration;

use axum::Router;
use axum::extract::Request;
use axum::response::IntoResponse;
use axum::routing::Route;
use axum::serve::Listener;
use futures_util::future::{Either, select};
use sync_wrapper::SyncWrapper;
use tokio::net::{TcpListener, ToSocketAddrs};
use tokio::sync::oneshot;
use tower_layer::Layer;
use tower_service::Service;

use super::layer::{
    BodyLimitLayer, CompressionLayer, CorsLayer, RequestIdLayer, TimeoutLayer, TraceLayer,
};
use crate::order::arrange;

/// How long [`Builder::serve`] waits, once told to stop, for the requests it serves to end.
const GRACE: Duration = Duration::from_secs(5);

/// What adds a layer to a router.
type Add = dyn FnOnce(Router) -> Router + Send + Sync;

/// What tells a server to stop.
type Signal = Pin<Box<dyn Future<Output = ()> + Send>>;

/// A builder of a server with no layer yet, which stops on Ctrl-C, or on `SIGTERM` on Unix.
///
/// ```no_run
/// use std::time::Duration;
///
/// use quoll::server;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// # async fn run() -> std::io::Result<()> {
/// server::builder()
///     .with(server::request_id())
///     .with(server::timeout(Duration::from_secs(10)))
///     .with(server::body_limit(1 << 20))
///     .serve("0.0.0.0:8080", api_router())
///     .await?;
/// # Ok(())
/// # }
/// ```
pub fn builder() -> Builder {
    Builder::default()
}

/// Serves a router, as `Api::router` gives it, with tower layers around it, the layers of quoll put in the order they
/// work in, and stops when it is told to.
///
/// The layers run in the order they are given, the first seeing the request first and the answer last, which is the
/// reverse of the order of `Router::layer`, except the layers of quoll, which take their places among themselves in
/// this order, from the outermost to the innermost, whatever the order they are given in:
///
/// 1. [`request_id`](super::request_id), so that every answer carries the id, a 503 of `timeout` or a 413 of
///    `body_limit` too;
/// 2. [`trace`](super::trace), whose span has the id, and which traces every answer;
/// 3. [`cors`](super::cors), whose headers every answer carries, so that a page reads a 503 or a 413 too;
/// 4. [`compression`](super::compression);
/// 5. [`timeout`](super::timeout);
/// 6. [`body_limit`](super::body_limit).
///
/// A layer of the application's keeps its place: the layers of quoll are sorted into the places they were given at,
/// and any other stays where it is. A layer of quoll's is told by its type.
///
/// It serves the router with [`serve`](Self::serve) or [`serve_on`](Self::serve_on), or gives it back with its
/// layers, [`into_router`](Self::into_router), to be served otherwise, or by [`test::spawn`](crate::test::spawn).
pub struct Builder {
    layers: Vec<(Option<Rank>, Box<Add>)>,
    shutdown: Option<SyncWrapper<Signal>>,
    grace: Duration,
}

impl Default for Builder {
    fn default() -> Self {
        Self {
            layers: Vec::new(),
            shutdown: None,
            grace: GRACE,
        }
    }
}

impl Builder {
    /// Adds `layer`: a layer of quoll's, at the place its kind works at, or any other tower layer, as a rate limit, at
    /// the place it is given.
    pub fn with<L>(mut self, layer: L) -> Self
    where
        L: Layer<Route> + Clone + Send + Sync + 'static,
        L::Service: Service<Request> + Clone + Send + Sync + 'static,
        <L::Service as Service<Request>>::Response: IntoResponse + 'static,
        <L::Service as Service<Request>>::Error: Into<Infallible> + 'static,
        <L::Service as Service<Request>>::Future: Send + 'static,
    {
        self.layers.push((
            rank::<L>(),
            Box::new(move |router: Router| router.layer(layer)),
        ));
        self
    }

    /// Stops the server when `signal` ends, in the place of Ctrl-C and `SIGTERM`: a test's channel, or
    /// `std::future::pending()` for a server that is never stopped.
    pub fn shutdown<F>(mut self, signal: F) -> Self
    where
        F: Future<Output = ()> + Send + 'static,
    {
        self.shutdown = Some(SyncWrapper::new(Box::pin(signal)));
        self
    }

    /// Waits up to `grace`, once the server is told to stop, for the requests it serves to end, 5 s unless it is set:
    /// a stream of `Events` or `JsonLines` may never end.
    pub fn grace(mut self, grace: Duration) -> Self {
        self.grace = grace;
        self
    }

    /// The router with the layers around it, for an application that serves it itself, or a test.
    pub fn into_router(self, router: Router) -> Router {
        layered(self.layers, router)
    }

    /// Binds `address`, as `"0.0.0.0:8080"`, then serves the router with the layers around it, as
    /// [`serve_on`](Self::serve_on) does.
    ///
    /// # Errors
    ///
    /// When `address` cannot be bound, or the signals of the system cannot be listened to.
    pub async fn serve(self, address: impl ToSocketAddrs, router: Router) -> io::Result<()> {
        let listener = TcpListener::bind(address).await?;

        self.serve_on(listener, router).await
    }

    /// Serves the router with the layers around it on `listener`, a `TcpListener` bound already, as on port 0, or a
    /// `UnixListener`, until it is told to stop: on Ctrl-C, or `SIGTERM` on Unix, which `docker stop` sends, unless
    /// [`shutdown`](Self::shutdown) says otherwise.
    ///
    /// Once told, it accepts no more connections, closes those that wait for a request, and returns when the requests
    /// it serves have ended, or once [`grace`](Self::grace) has passed: the connections still open then end with the
    /// tokio runtime, as when `main` returns.
    ///
    /// ```no_run
    /// use quoll::server;
    ///
    /// # fn api_router() -> axum::Router { axum::Router::new() }
    /// # async fn run() -> std::io::Result<()> {
    /// let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
    /// let address = listener.local_addr()?;
    ///
    /// println!("serving on {address}");
    /// server::builder().serve_on(listener, api_router()).await?;
    /// # Ok(())
    /// # }
    /// ```
    ///
    /// # Errors
    ///
    /// When the signals of the system cannot be listened to.
    pub async fn serve_on<L>(self, listener: L, router: Router) -> io::Result<()>
    where
        L: Listener,
        L::Addr: fmt::Debug,
    {
        let Self {
            layers,
            shutdown,
            grace,
        } = self;
        let signal = match shutdown {
            Some(signal) => signal.into_inner(),
            None => Box::pin(terminated()?),
        };
        let (stopping, stopped) = oneshot::channel::<()>();
        let signal = async move {
            signal.await;
            // The receiver is gone once the server has returned, when there is no deadline left to start.
            let _ = stopping.send(());
        };
        let serving = axum::serve(listener, layered(layers, router))
            .with_graceful_shutdown(signal)
            .into_future();
        let deadline = async move {
            match stopped.await {
                Ok(()) => tokio::time::sleep(grace).await,
                Err(_) => pending().await,
            }
        };

        match select(pin!(serving), pin!(deadline)).await {
            Either::Left((served, _)) => served,
            Either::Right(((), _)) => Ok(()),
        }
    }
}

/// Shows its layers by their number, as they have no `Debug` once added.
impl fmt::Debug for Builder {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let shutdown = if self.shutdown.is_some() {
            "the application's signal"
        } else {
            "Ctrl-C and SIGTERM"
        };

        f.debug_struct("Builder")
            .field("layers", &self.layers.len())
            .field("shutdown", &shutdown)
            .field("grace", &self.grace)
            .finish()
    }
}

/// Where a layer of quoll's goes among the others a [`Builder`] is given, from the outermost to the innermost: the
/// order the layers work in.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Rank {
    RequestId,
    Trace,
    Cors,
    Compression,
    Timeout,
    BodyLimit,
}

/// The rank of a layer of quoll's, told by its type, or `None` for any other.
fn rank<L: 'static>() -> Option<Rank> {
    let layer = TypeId::of::<L>();

    [
        (TypeId::of::<RequestIdLayer>(), Rank::RequestId),
        (TypeId::of::<TraceLayer>(), Rank::Trace),
        (TypeId::of::<CorsLayer>(), Rank::Cors),
        (TypeId::of::<CompressionLayer>(), Rank::Compression),
        (TypeId::of::<TimeoutLayer>(), Rank::Timeout),
        (TypeId::of::<BodyLimitLayer>(), Rank::BodyLimit),
    ]
    .into_iter()
    .find_map(|(id, rank)| (id == layer).then_some(rank))
}

/// `router` with `layers` around it, the first given the outermost: `Router::layer` puts each layer around the
/// others, so they are added from the innermost.
fn layered(layers: Vec<(Option<Rank>, Box<Add>)>, router: Router) -> Router {
    arrange(layers)
        .into_iter()
        .rev()
        .fold(router, |router, add| add(router))
}

/// Ends on Ctrl-C, or `SIGTERM`, which `docker stop` and Kubernetes send.
#[cfg(unix)]
fn terminated() -> io::Result<impl Future<Output = ()> + Send> {
    use tokio::signal::unix::{SignalKind, signal};

    let mut interrupt = signal(SignalKind::interrupt())?;
    let mut terminate = signal(SignalKind::terminate())?;

    Ok(async move {
        select(pin!(interrupt.recv()), pin!(terminate.recv())).await;
    })
}

/// Ends on Ctrl-C.
#[cfg(not(unix))]
fn terminated() -> io::Result<impl Future<Output = ()> + Send> {
    let mut ctrl_c = tokio::signal::windows::ctrl_c()?;

    Ok(async move {
        ctrl_c.recv().await;
    })
}
