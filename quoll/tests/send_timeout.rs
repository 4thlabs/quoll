//! The bound on how long a write of the server waits for its socket to take bytes: a client on raw TCP that reads the
//! head of an answer, then nothing, is cut, and its handler's stream dropped; a slow reader, a quiet stream and a
//! server without the bound keep theirs.
//!
//! The kernel's buffers absorb several MiB of an answer before a write waits, about 6 MiB on loopback: the clock runs
//! only from then on. The default of 60 s is tested under tokio's paused clock, which jumps to the cut once the server
//! has nothing to do but wait for the client; a slow reader in real time, as the paused clock would jump its pauses
//! too.

#![cfg(feature = "server")]

use std::io;
use std::net::SocketAddr;
use std::pin::{Pin, pin};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::task::{Context, Poll};
use std::time::Duration;

use axum::serve::Listener;
use futures_util::{Stream, StreamExt, stream};
use quoll::{ByteStream, Events, JsonLines, Response, Result, server};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, ReadBuf};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{Notify, mpsc};
use tokio::time::Instant;

mod support {
    pub(crate) mod log;
}

quoll::api! {
    api Api = "/api";

    group Feeds {
        GET "/events"       => events() -> Events<String>;
        GET "/lines"        => lines() -> JsonLines<String>;
        GET "/bytes"        => bytes() -> ByteStream;
        GET "/quiet/events" => quiet_events() -> Events<String>;
        GET "/quiet/lines"  => quiet_lines() -> JsonLines<String>;
    }
}

/// Far past the bound of 60 s and what the buffers take to fill, under the paused clock.
const LONG: Duration = Duration::from_secs(600);

/// Far past the bound of 1 s and what the buffers take to fill, in real time: about 1.6 s.
const REAL_LONG: Duration = Duration::from_secs(10);

/// A flag that is set once, and awaited.
#[derive(Debug, Default)]
struct Flag {
    is_set: AtomicBool,
    changed: Notify,
}

impl Flag {
    fn set(&self) {
        self.is_set.store(true, Ordering::SeqCst);
        self.changed.notify_waiters();
    }

    fn is_set(&self) -> bool {
        self.is_set.load(Ordering::SeqCst)
    }

    /// Whether it is set within `bound`.
    async fn within(&self, bound: Duration) -> bool {
        let set = async {
            loop {
                let mut changed = pin!(self.changed.notified());

                changed.as_mut().enable();

                if self.is_set() {
                    return;
                }

                changed.await;
            }
        };

        tokio::time::timeout(bound, set).await.is_ok()
    }
}

/// The service: each stream is fed by a producer that sends lines as fast as a channel of 64 takes them, and whose
/// send fails once the stream is dropped, as an application's producer does. The quiet ones send one line, then
/// nothing. `gone` is set when the stream is dropped.
#[derive(Clone)]
struct App {
    gone: Arc<Flag>,
}

/// A router of the contract, and the flag its stream sets when it is dropped.
fn router() -> (axum::Router, Arc<Flag>) {
    let gone = Arc::new(Flag::default());
    let router = Api::router(ApiServices {
        feeds: App {
            gone: Arc::clone(&gone),
        },
    });

    (router, gone)
}

fn produced(gone: &Arc<Flag>) -> impl Stream<Item = String> + Send + 'static {
    let (sender, mut receiver) = mpsc::channel::<String>(64);
    let gone = Arc::clone(gone);

    tokio::spawn(async move {
        while sender.send("x".repeat(100)).await.is_ok() {}

        gone.set();
    });

    stream::poll_fn(move |cx| receiver.poll_recv(cx))
}

/// Sets its flag when the stream holding it is dropped.
struct SetOnDrop(Arc<Flag>);

impl Drop for SetOnDrop {
    fn drop(&mut self) {
        self.0.set();
    }
}

fn quiet(gone: &Arc<Flag>) -> impl Stream<Item = String> + Send + 'static {
    let on_drop = SetOnDrop(Arc::clone(gone));

    stream::once(async { "first".to_owned() })
        .chain(stream::pending())
        .map(move |line| {
            let _ = &on_drop;
            line
        })
}

impl FeedsServer for App {
    async fn events(&self) -> Result<Response<Events<String>>> {
        Ok(Response::new(Events::new(produced(&self.gone))))
    }

    async fn lines(&self) -> Result<Response<JsonLines<String>>> {
        Ok(Response::new(JsonLines::new(produced(&self.gone))))
    }

    async fn bytes(&self) -> Result<Response<ByteStream>> {
        let bytes = produced(&self.gone).map(String::into_bytes);

        Ok(Response::new(ByteStream::new(bytes)))
    }

    async fn quiet_events(&self) -> Result<Response<Events<String>>> {
        Ok(Response::new(Events::new(quiet(&self.gone))))
    }

    async fn quiet_lines(&self) -> Result<Response<JsonLines<String>>> {
        Ok(Response::new(JsonLines::new(quiet(&self.gone))))
    }
}

/// A free port of 127.0.0.1.
async fn bound() -> (TcpListener, SocketAddr) {
    let listener = TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port of 127.0.0.1 is free");
    let address = listener
        .local_addr()
        .expect("a bound listener has an address");

    (listener, address)
}

/// Serves `router` with `builder`, never told to stop, on a task.
fn serve<L>(builder: server::Builder, listener: L, router: axum::Router)
where
    L: Listener,
    L::Addr: std::fmt::Debug,
{
    let serving = builder
        .shutdown(std::future::pending())
        .serve_on(listener, router);

    tokio::spawn(serving);
}

/// Sends a `GET` of `path` on raw TCP and reads the head of the answer, then nothing: the body stays in the kernel's
/// buffers, as with a client that stops reading.
async fn stalled(address: SocketAddr, path: &str) -> TcpStream {
    let mut stream = TcpStream::connect(address)
        .await
        .expect("the server accepts");
    let request = format!("GET {path} HTTP/1.1\r\nHost: test\r\n\r\n");

    stream
        .write_all(request.as_bytes())
        .await
        .expect("the request is sent");

    let mut head = Vec::new();
    let mut byte = [0; 1];

    while !head.ends_with(b"\r\n\r\n") {
        stream
            .read_exact(&mut byte)
            .await
            .expect("the head is read");
        head.push(byte[0]);
    }

    assert!(
        head.starts_with(b"HTTP/1.1 200"),
        "{}",
        String::from_utf8_lossy(&head)
    );
    stream
}

/// Reads what is left on `stream` until the server's end: the bytes read, and whether it ended in an error rather
/// than a clean close.
async fn read_to_end(stream: &mut TcpStream) -> (usize, bool) {
    let mut buffer = vec![0; 1 << 16];
    let mut read = 0;

    loop {
        match stream.read(&mut buffer).await {
            Ok(0) => return (read, false),
            Ok(n) => read += n,
            Err(_) => return (read, true),
        }
    }
}

/// A listener whose connections say when a write first waits for the client, and when hyper drops them.
struct Watched {
    listener: TcpListener,
    refused: Arc<Flag>,
    closed: Arc<Flag>,
}

impl Watched {
    /// The listener, its address, and the flags of its connections, for a test that opens one.
    async fn bind() -> (Self, SocketAddr, Arc<Flag>, Arc<Flag>) {
        let (listener, address) = bound().await;
        let refused = Arc::new(Flag::default());
        let closed = Arc::new(Flag::default());
        let watched = Self {
            listener,
            refused: Arc::clone(&refused),
            closed: Arc::clone(&closed),
        };

        (watched, address, refused, closed)
    }
}

impl Listener for Watched {
    type Io = Connection;
    type Addr = SocketAddr;

    async fn accept(&mut self) -> (Connection, SocketAddr) {
        let (stream, address) = loop {
            if let Ok(accepted) = self.listener.accept().await {
                break accepted;
            }
        };
        let connection = Connection {
            stream,
            refused: Arc::clone(&self.refused),
            closed: Arc::clone(&self.closed),
        };

        (connection, address)
    }

    fn local_addr(&self) -> io::Result<SocketAddr> {
        self.listener.local_addr()
    }
}

/// A connection of a [`Watched`] listener.
struct Connection {
    stream: TcpStream,
    refused: Arc<Flag>,
    closed: Arc<Flag>,
}

impl Drop for Connection {
    fn drop(&mut self) {
        self.closed.set();
    }
}

impl Connection {
    fn watched<T>(&self, poll: Poll<io::Result<T>>) -> Poll<io::Result<T>> {
        if poll.is_pending() {
            self.refused.set();
        }

        poll
    }
}

impl AsyncRead for Connection {
    fn poll_read(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_read(cx, buf)
    }
}

impl AsyncWrite for Connection {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        let poll = Pin::new(&mut self.stream).poll_write(cx, buf);

        self.watched(poll)
    }

    fn poll_write_vectored(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<io::Result<usize>> {
        let poll = Pin::new(&mut self.stream).poll_write_vectored(cx, bufs);

        self.watched(poll)
    }

    fn is_write_vectored(&self) -> bool {
        self.stream.is_write_vectored()
    }

    fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_flush(cx)
    }

    fn poll_shutdown(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.stream).poll_shutdown(cx)
    }
}

/// Every stream to a client that stops reading is cut after the bound, its handler's stream dropped, and the
/// connection reset: the client reads what its own buffer held, then an error. Without the bound, the stream would
/// never be dropped.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_stalled_client_is_cut_after_the_bound_and_its_stream_dropped() {
    const BOUND: Duration = Duration::from_secs(1);

    let cut = |path: &'static str| async move {
        let (router, gone) = router();
        let (listener, address) = bound().await;

        serve(server::builder().send_timeout(BOUND), listener, router);

        let mut client = stalled(address, path).await;
        let stopped = Instant::now();

        assert!(
            gone.within(REAL_LONG).await,
            "{path}: the stream is dropped"
        );

        let dropped_after = stopped.elapsed();
        let (read, failed) = read_to_end(&mut client).await;

        println!(
            "{path}: dropped {dropped_after:?} after the client stopped reading, which then read {} KiB and {}",
            read / 1024,
            if failed { "an error" } else { "a close" }
        );
        assert!(dropped_after >= BOUND, "{path}: not before the bound");
        assert!(failed, "{path}: the connection is reset");
    };

    tokio::join!(cut("/api/lines"), cut("/api/events"), cut("/api/bytes"));
}

#[tokio::test(start_paused = true)]
async fn the_bound_is_60_s_unless_it_is_set() {
    let (router, gone) = router();
    let (listener, address) = bound().await;

    serve(server::builder(), listener, router);

    let _client = stalled(address, "/api/lines").await;
    let stopped = Instant::now();

    assert!(gone.within(LONG).await, "the stream is dropped");
    assert!(stopped.elapsed() >= Duration::from_secs(60));
}

#[tokio::test(start_paused = true)]
async fn test_spawn_has_the_same_bound() {
    let (router, gone) = router();
    let url = quoll::test::spawn(router).await;
    let address = url
        .trim_start_matches("http://")
        .parse()
        .expect("the URL holds an address");
    let _client = stalled(address, "/api/lines").await;
    let stopped = Instant::now();

    assert!(gone.within(LONG).await, "the stream is dropped");
    assert!(stopped.elapsed() >= Duration::from_secs(60));
}

#[tokio::test(start_paused = true)]
async fn the_cut_is_logged_once_with_the_address_of_the_client() {
    let (log, _capture) = support::log::capture(false);
    let (router, gone) = router();
    let (listener, address) = bound().await;

    serve(server::builder(), listener, router);

    let client = stalled(address, "/api/lines").await;
    let peer = client
        .local_addr()
        .expect("a connected stream has an address");

    assert!(gone.within(LONG).await, "the stream is dropped");

    let text = log.text();
    let line = format!(
        " INFO quoll::server::send_timeout: a connection was closed: it took no bytes of the answer for 60s \
         peer={peer} bound=60s\n"
    );

    assert_eq!(text.matches("a connection was closed").count(), 1, "{text}");
    assert!(text.contains(&line), "{text}");
}

#[tokio::test(start_paused = true)]
async fn without_send_timeout_a_stalled_client_is_kept() {
    let (router, gone) = router();
    let (listener, address, refused, closed) = Watched::bind().await;

    serve(server::builder().without_send_timeout(), listener, router);

    let _client = stalled(address, "/api/lines").await;

    assert!(refused.within(LONG).await, "a write waits for the client");

    tokio::time::sleep(Duration::from_secs(3600)).await;

    assert!(!gone.is_set(), "the stream is kept");
    assert!(!closed.is_set(), "the connection is kept");
}

/// Every answer is bounded, not the streams alone: a large one to a client that stops reading is cut too. The
/// connection is no tokio `TcpStream` here, but the test's own: it is closed, not reset.
#[tokio::test(start_paused = true)]
async fn a_large_answer_to_a_stalled_client_is_cut_too() {
    const LARGE: usize = 32 << 20;

    let router =
        axum::Router::new().route("/large", axum::routing::get(|| async { vec![b'x'; LARGE] }));
    let (listener, address, refused, closed) = Watched::bind().await;

    serve(server::builder(), listener, router);

    let mut client = stalled(address, "/large").await;

    assert!(refused.within(LONG).await, "a write waits for the client");

    let refused_at = Instant::now();

    assert!(closed.within(LONG).await, "the connection is closed");
    assert!(refused_at.elapsed() >= Duration::from_secs(60));

    let (read, failed) = read_to_end(&mut client).await;

    assert!(read < LARGE, "the answer is cut, {read} bytes read");
    assert!(!failed, "the connection is closed, not reset");
}

/// A stream with nothing to send runs no clock: the quiet `JsonLines` sends nothing after its first line, and the
/// keep-alive comments of the quiet `Events`, a few bytes every 15 s, never fill the buffers.
#[tokio::test(start_paused = true)]
async fn a_quiet_stream_is_kept() {
    for path in ["/api/quiet/lines", "/api/quiet/events"] {
        let (router, gone) = router();
        let (listener, address, _refused, closed) = Watched::bind().await;

        serve(server::builder(), listener, router);

        let _client = stalled(address, path).await;

        tokio::time::sleep(Duration::from_secs(3600)).await;

        assert!(!gone.is_set(), "{path}: the stream is kept");
        assert!(!closed.is_set(), "{path}: the connection is kept");
    }
}

/// A reader that takes bytes all the time, if slowly, is kept. The kernel wakes a writer once a good part of its
/// queue has gone, about a third of a queue that grows to 7 MiB on loopback: the server sees a reader progress by
/// hundreds of KiB at a time, so this one takes 2 MiB every 300 ms. Each wait of the server is then its pause and its
/// read, well under 1 s here; the bound of 3 s leaves room for a loaded runner, whose reads and wake-ups lag.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_slow_reader_is_kept() {
    const BOUND: Duration = Duration::from_secs(3);
    const CHUNK: usize = 2 << 20;

    let (router, gone) = router();
    let (listener, address) = bound().await;

    serve(server::builder().send_timeout(BOUND), listener, router);

    let mut client = stalled(address, "/api/lines").await;
    let mut buffer = vec![0; CHUNK];

    for _ in 0..8 {
        tokio::time::sleep(Duration::from_millis(300)).await;
        client
            .read_exact(&mut buffer)
            .await
            .expect("the slow reader reads on");
    }

    assert!(!gone.is_set(), "the stream is kept");
}
