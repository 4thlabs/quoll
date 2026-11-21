//! The builder of a server: the layers of quoll in the order they work in, any other layer where it is given, and the
//! loop that serves a router until it is told to stop.

use std::any::TypeId;
use std::convert::Infallible;
use std::fmt;
use std::future::{Future, IntoFuture};
use std::io;
use std::net::SocketAddr;
use std::pin::{Pin, pin};
use std::time::Duration;

use axum::Router;
use axum::extract::Request;
use axum::middleware::map_response;
use axum::response::{IntoResponse, Response};
use axum::routing::Route;
use axum::serve::Listener;
use futures_core::Stream;
use futures_util::future::{Either, select};
use futures_util::{StreamExt, stream};
use sync_wrapper::SyncWrapper;
use tokio::sync::{oneshot, watch};
use tower_layer::Layer;
use tower_service::Service;

use super::layer::{
    BodyLimitLayer, CompressionLayer, CorsLayer, RequestIdLayer, TimeoutLayer, TraceLayer,
};
use crate::__private::server::ended_on_stop;
use crate::order::arrange;

/// How long [`Builder::serve`] waits, once told to stop, for the requests it serves to end.
const GRACE: Duration = Duration::from_secs(5);

/// What adds a layer to a router.
type Add = dyn FnOnce(Router) -> Router + Send + Sync;

/// What tells a server to stop.
type Signal = Pin<Box<dyn Future<Output = ()> + Send>>;

/// The signals a server is told to stop by: the first starts its grace, the next ends it.
type Signals = Pin<Box<dyn Stream<Item = ()> + Send>>;

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

/// Serves a router with tower layers around it, until it is told to stop.
///
/// The router is the one `Api::router` gives, and the layers of quoll are put in the order they work in.
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
    ///
    /// ```
    /// use std::time::Duration;
    ///
    /// use quoll::server;
    /// use tokio::sync::oneshot;
    ///
    /// # fn api_router() -> axum::Router { axum::Router::new() }
    /// # #[tokio::main(flavor = "current_thread")]
    /// # async fn main() -> std::io::Result<()> {
    /// let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await?;
    /// let (stop, stopped) = oneshot::channel::<()>();
    ///
    /// let served = tokio::spawn(
    ///     server::builder()
    ///         .shutdown(async move {
    ///             let _ = stopped.await;
    ///         })
    ///         .grace(Duration::from_secs(1))
    ///         .serve_on(listener, api_router()),
    /// );
    ///
    /// stop.send(()).unwrap();
    /// served.await.unwrap()?;
    /// # Ok(())
    /// # }
    /// ```
    pub fn shutdown<F>(mut self, signal: F) -> Self
    where
        F: Future<Output = ()> + Send + 'static,
    {
        self.shutdown = Some(SyncWrapper::new(Box::pin(signal)));
        self
    }

    /// Waits up to `grace`, once the server is told to stop, for the requests it serves to end, 5 s unless it is set.
    ///
    /// The streams of `Events`, `JsonLines` and `ByteStream` end at once, after the piece each is sending, as
    /// [`serve_on`](Self::serve_on) says: the grace is for the requests a handler is still answering, and for a body
    /// that never ends of a route that is not a contract's.
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
    pub async fn serve(self, address: impl Address, router: Router) -> io::Result<()> {
        let listener = address.bind().await?;

        self.serve_on(listener, router).await
    }

    /// Serves the router with the layers around it on `listener`, a `TcpListener` bound already, as on port 0, or a
    /// `UnixListener`, until it is told to stop: on Ctrl-C, or `SIGTERM` on Unix, which `docker stop` sends, unless
    /// [`shutdown`](Self::shutdown) says otherwise.
    ///
    /// Once told, it accepts no more connections, closes those that wait for a request, and returns `Ok` when the
    /// requests it serves have ended. It returns `Ok` too once [`grace`](Self::grace) has passed, or when it is told a
    /// second time, by a second Ctrl-C or `SIGTERM`, and logs it at `WARN`: the connections still open are left to the
    /// tokio runtime, and their handlers may still run after it has returned, until the runtime ends, as when `main`
    /// returns.
    ///
    /// The answers of the routes whose result is a stream, `Events`, `JsonLines` and `ByteStream`, end as soon as it is
    /// told, after the event, the line or the chunk each is sending: the client, and any proxy between, reads a clean
    /// end, as when the handler's stream ends, rather than a connection cut once the grace has passed. An `Events`
    /// stream ends with nothing more, no `event: problem`: a stop is no failure. A router served otherwise, by
    /// `axum::serve` or [`test::spawn`](crate::test::spawn), leaves its streams to run until their connection closes.
    ///
    /// Ctrl-C and `SIGTERM` are tokio's from the first call on, and stay so once it has returned: the process does not
    /// get their default action back, and a later Ctrl-C does not end it.
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
        let signals: Signals = match shutdown {
            Some(signal) => Box::pin(stream::once(signal.into_inner()).chain(stream::pending())),
            None => terminated()?,
        };
        let (stopping, told) = watch::channel(false);

        // Around the router, inside every other layer: they see the stream's body end as when its handler's stream
        // ends.
        let router = router.layer(map_response(move |response: Response| {
            let response = ended_on_stop(response, &told);

            async move { response }
        }));

        run(listener, layered(layers, router), signals, stopping, grace).await
    }
}

/// An address [`Builder::serve`] binds: a `&str` or a `String`, as `"0.0.0.0:8080"` or `"localhost:8080"`, whose host
/// is looked up without blocking, or a `SocketAddr`.
pub trait Address: sealed::Bind {}

impl Address for &str {}

impl Address for String {}

impl Address for SocketAddr {}

mod sealed {
    use std::future::Future;
    use std::io;
    use std::net::SocketAddr;

    use tokio::net::TcpListener;

    /// How an address is bound, which no application calls.
    pub trait Bind: Send {
        /// A listener bound to the address.
        fn bind(self) -> impl Future<Output = io::Result<TcpListener>> + Send;
    }

    impl Bind for &str {
        fn bind(self) -> impl Future<Output = io::Result<TcpListener>> + Send {
            TcpListener::bind(self)
        }
    }

    impl Bind for String {
        async fn bind(self) -> io::Result<TcpListener> {
            TcpListener::bind(self.as_str()).await
        }
    }

    impl Bind for SocketAddr {
        fn bind(self) -> impl Future<Output = io::Result<TcpListener>> + Send {
            TcpListener::bind(self)
        }
    }
}

/// Serves `router` on `listener` until the first of `signals`, then for the requests it serves, up to `grace` or the
/// next of `signals`. At the first of `signals`, `stopping` is set to `true`, which ends the streams in flight.
async fn run<L>(
    listener: L,
    router: Router,
    mut signals: Signals,
    stopping: watch::Sender<bool>,
    grace: Duration,
) -> io::Result<()>
where
    L: Listener,
    L::Addr: fmt::Debug,
{
    let (stop, stopped) = oneshot::channel::<()>();
    let serving = axum::serve(listener, router)
        .with_graceful_shutdown(async move {
            // The sender lives until the server is told to stop, or has returned.
            let _ = stopped.await;
        })
        .into_future();
    let mut serving = pin!(serving);

    if let Either::Left((served, _)) = select(serving.as_mut(), signals.next()).await {
        return served;
    }

    // The receiver lives in the server's future, which has not returned: the send cannot fail.
    let _ = stop.send(());

    // Whether any stream is in flight or not.
    stopping.send_replace(true);

    let deadline = pin!(tokio::time::sleep(grace));

    match select(serving, select(deadline, signals.next())).await {
        Either::Left((served, _)) => served,
        Either::Right((Either::Left(_), _)) => {
            tracing::warn!(
                ?grace,
                "the grace ran out with connections still open: they are left to the runtime, their handlers may still run"
            );
            Ok(())
        }
        Either::Right((Either::Right(_), _)) => {
            tracing::warn!(
                "a second signal ended the grace with connections still open: they are left to the runtime, their handlers may still run"
            );
            Ok(())
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

/// Ctrl-C and `SIGTERM`, which `docker stop` and Kubernetes send, each time they come.
#[cfg(unix)]
fn terminated() -> io::Result<Signals> {
    use tokio::signal::unix::{SignalKind, signal};

    let interrupt = signal(SignalKind::interrupt())?;
    let terminate = signal(SignalKind::terminate())?;

    Ok(Box::pin(stream::unfold(
        (interrupt, terminate),
        |(mut interrupt, mut terminate)| async move {
            {
                let interrupted = pin!(interrupt.recv());
                let terminated = pin!(terminate.recv());

                select(interrupted, terminated).await;
            }

            Some(((), (interrupt, terminate)))
        },
    )))
}

/// Ctrl-C, each time it comes.
#[cfg(not(unix))]
fn terminated() -> io::Result<Signals> {
    let ctrl_c = tokio::signal::windows::ctrl_c()?;

    Ok(Box::pin(stream::unfold(ctrl_c, |mut ctrl_c| async move {
        ctrl_c.recv().await;
        Some(((), ctrl_c))
    })))
}

#[cfg(test)]
mod tests {
    use axum::body::{Body, Bytes};
    use tokio::net::TcpListener;
    use tokio::sync::mpsc;

    use super::*;

    /// Signals sent through a channel, as the system sends Ctrl-C: the sender, and what the server is told by.
    fn sent() -> (mpsc::UnboundedSender<()>, Signals) {
        let (send, received) = mpsc::unbounded_channel();
        let signals = stream::unfold(received, |mut received| async move {
            received.recv().await.map(|()| ((), received))
        });

        (send, Box::pin(signals))
    }

    #[tokio::test]
    async fn a_second_signal_ends_the_grace_at_once() {
        let router = Router::new().route(
            "/endless",
            axum::routing::get(|| async {
                Body::from_stream(stream::pending::<io::Result<Bytes>>())
            }),
        );
        let listener = TcpListener::bind("127.0.0.1:0")
            .await
            .expect("a port of 127.0.0.1 is free");
        let url = format!(
            "http://{}/endless",
            listener
                .local_addr()
                .expect("a bound listener has an address")
        );
        let (signal, signals) = sent();
        let (stopping, _) = watch::channel(false);
        let serving = tokio::spawn(run(
            listener,
            router,
            signals,
            stopping,
            Duration::from_secs(3600),
        ));
        let answer = reqwest::get(url).await.expect("the server answers");

        signal.send(()).expect("the server listens");
        signal.send(()).expect("the server listens");

        // The grace is an hour: only the second signal ends it before this bound.
        tokio::time::timeout(Duration::from_secs(10), serving)
            .await
            .expect("the second signal ends the grace")
            .expect("the server's task does not panic")
            .expect("the server stops with no error");
        drop(answer);
    }
}
