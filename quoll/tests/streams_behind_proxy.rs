//! The streams of a server, `Events`, `JsonLines` and `ByteStream`, when `server::builder()`'s server is told to stop:
//! server-sent events seen to open before their first event, through a proxy that holds the head of an answer until
//! its first bytes, as Vite's does, then ended cleanly at the stop, long before the grace; a `JsonLines` and a
//! `ByteStream` left to run until the grace has passed, then cut.

#![cfg(all(feature = "server", feature = "client"))]

use std::io;
use std::net::SocketAddr;
use std::thread;
use std::time::Duration;

use futures_util::{StreamExt, stream};
use quoll::rest::{self, ByteStream, Events, JsonLines, Result};
use quoll::server;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::oneshot;

use crate::support::log::Log;

mod support {
    pub(crate) mod log;
}

rest::api! {
    api Api = "/api";

    group Feeds {
        GET "/events" => events() -> Events<u32>;
        GET "/silent" => silent() -> Events<u32>;
        GET "/lines"  => lines() -> JsonLines<u32>;
        GET "/bytes"  => bytes() -> ByteStream;
    }
}

/// The bound a test waits for what it checks: far past what it takes, far short of a grace of an hour.
const BOUND: Duration = Duration::from_secs(10);

/// The grace of a server whose `Events` end at its stop: what ends before it is not ended by it.
const HOUR: Duration = Duration::from_secs(3600);

/// The grace of a server that cuts its `JsonLines` and `ByteStream`: long enough for its `Events` to end well before,
/// short enough for a test to wait it out.
const GRACE: Duration = Duration::from_secs(2);

/// The service: each stream sends one value, or none for `silent`, then never ends.
struct App;

/// `first`, then a stream that never ends.
fn held<T: Send + 'static>(
    first: Option<T>,
) -> impl futures_util::Stream<Item = T> + Send + 'static {
    stream::iter(first).chain(stream::pending())
}

impl FeedsServer for App {
    async fn events(&self) -> Result<Events<u32>> {
        Ok(Events::new(held(Some(1))))
    }

    async fn silent(&self) -> Result<Events<u32>> {
        Ok(Events::new(held(None)))
    }

    async fn lines(&self) -> Result<JsonLines<u32>> {
        Ok(JsonLines::new(held(Some(1))))
    }

    async fn bytes(&self) -> Result<ByteStream> {
        Ok(ByteStream::new(held(Some(b"chunk\n".to_vec()))))
    }
}

/// A listener on a free port of 127.0.0.1, and its address.
async fn bound() -> (TcpListener, SocketAddr) {
    let listener = TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port of 127.0.0.1 is free");
    let address = listener
        .local_addr()
        .expect("a bound listener has an address");

    (listener, address)
}

/// The api served by `server::builder()` with a grace of an hour, on a task: its address, the sender that stops it,
/// and the task, which ends with what `serve_on` returned.
async fn served() -> (
    SocketAddr,
    oneshot::Sender<()>,
    tokio::task::JoinHandle<io::Result<()>>,
) {
    let (listener, address) = bound().await;
    let (stop, stopped) = oneshot::channel::<()>();
    let builder = server::builder().grace(HOUR).shutdown(async move {
        let _ = stopped.await;
    });
    let router = Api::router(ApiServices { feeds: App });

    (
        address,
        stop,
        tokio::spawn(builder.serve_on(listener, router)),
    )
}

/// Waits for the server to return, which it must do well before its grace of an hour, and checks that it said nothing
/// of connections left open.
async fn returned(serving: tokio::task::JoinHandle<io::Result<()>>, log: &Log) {
    tokio::time::timeout(BOUND, serving)
        .await
        .expect("the server returns before its grace")
        .expect("the server's task does not panic")
        .expect("the server stops with no error");

    let log = log.text();

    assert!(!log.contains(" WARN "), "{log}");
}

/// What a server on a thread of its own returned, with the log of its thread, once its runtime has ended.
type Returned = oneshot::Receiver<(io::Result<()>, Log)>;

/// The api served by `server::builder()` with `grace`, on a thread of its own, whose runtime ends once `serve_on` has
/// returned, as a server's does when `main` returns: what the server leaves open is cut then. Its address, the sender
/// that stops it, and what `serve_on` returned with the log of its thread, once its runtime has ended.
fn served_on_a_thread(grace: Duration) -> (SocketAddr, oneshot::Sender<()>, Returned) {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("a port of 127.0.0.1 is free");
    let address = listener
        .local_addr()
        .expect("a bound listener has an address");
    let (stop, stopped) = oneshot::channel::<()>();
    let (done, returned) = oneshot::channel();

    listener
        .set_nonblocking(true)
        .expect("a listener can be made non-blocking");
    thread::spawn(move || {
        let (log, _capture) = support::log::capture(false);
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("a runtime builds");
        let served = runtime.block_on(async move {
            let listener = TcpListener::from_std(listener)?;
            let builder = server::builder().grace(grace).shutdown(async move {
                let _ = stopped.await;
            });

            builder
                .serve_on(listener, Api::router(ApiServices { feeds: App }))
                .await
        });

        // The connections the server left open end with its runtime.
        drop(runtime);
        let _ = done.send((served, log));
    });

    (address, stop, returned)
}

/// A proxy on a free port of 127.0.0.1 to `upstream`, as Vite's holds the head of each answer until the first bytes
/// of its body: its URL.
async fn holding_proxy(upstream: SocketAddr) -> String {
    let (listener, address) = bound().await;

    tokio::spawn(async move {
        while let Ok((client, _)) = listener.accept().await {
            tokio::spawn(forwarded(client, upstream));
        }
    });

    format!("http://{address}")
}

/// Forwards `client`'s requests to `upstream` as they come, and the answers back once their body has begun.
async fn forwarded(client: TcpStream, upstream: SocketAddr) -> io::Result<()> {
    let server = TcpStream::connect(upstream).await?;
    let (mut from_client, mut to_client) = client.into_split();
    let (mut from_server, mut to_server) = server.into_split();

    tokio::spawn(async move { tokio::io::copy(&mut from_client, &mut to_server).await });

    let mut held = Vec::new();

    // One answer is enough: these tests send one request per connection they care about.
    loop {
        let mut buffer = [0; 4096];
        let read = from_server.read(&mut buffer).await?;

        if read == 0 {
            return Ok(());
        }

        held.extend_from_slice(&buffer[..read]);

        let head = held.windows(4).position(|end| end == b"\r\n\r\n");

        if head.is_some_and(|head| held.len() > head + 4) {
            break;
        }
    }

    to_client.write_all(&held).await?;
    tokio::io::copy(&mut from_server, &mut to_client).await?;

    // The upstream closed: the client's side ends too, as Vite's proxy ends it after a clean end.
    to_client.shutdown().await
}

#[tokio::test]
async fn events_are_seen_to_open_and_end_through_a_proxy_that_holds_the_head() -> Result<()> {
    let (log, _capture) = support::log::capture(false);
    let (address, stop, serving) = served().await;
    let api = Api::client(&holding_proxy(address).await)?;

    // No event ever comes: the call returns once the head is through, which the opening comment lets out.
    let mut events = tokio::time::timeout(BOUND, api.feeds().silent())
        .await
        .expect("the stream is seen to open before any event")?;

    stop.send(()).expect("the server waits for its signal");

    let end = tokio::time::timeout(BOUND, events.next())
        .await
        .expect("the stream ends when the server stops");

    assert!(end.is_none(), "a clean end, not {end:?}");
    returned(serving, &log).await;
    Ok(())
}

#[tokio::test]
async fn at_the_stop_events_end_cleanly_while_lines_and_bytes_are_cut_once_the_grace_has_passed()
-> Result<()> {
    let (address, stop, mut done) = served_on_a_thread(GRACE);
    let api = Api::client(&format!("http://{address}"))?;
    let mut events = api.feeds().events().await?;
    let mut lines = api.feeds().lines().await?;
    let mut bytes = api.feeds().bytes().await?;

    // Each stream is in flight, its first item read.
    assert_eq!(events.next().await.transpose()?, Some(1));
    assert_eq!(lines.next().await.transpose()?, Some(1));
    assert_eq!(bytes.next().await.transpose()?, Some(b"chunk\n".to_vec()));

    stop.send(()).expect("the server waits for its signal");

    let end = tokio::time::timeout(BOUND, events.next())
        .await
        .expect("the events end when the server stops");

    assert!(end.is_none(), "the events end cleanly, not {end:?}");

    // The lines and the bytes still hold the server, which waits for them until its grace has passed.
    assert!(
        matches!(done.try_recv(), Err(oneshot::error::TryRecvError::Empty)),
        "the server still waits for the lines and the bytes"
    );

    let (line, chunk) = tokio::time::timeout(
        BOUND,
        futures_util::future::join(lines.next(), bytes.next()),
    )
    .await
    .expect("the lines and the bytes are cut once the grace has passed");

    assert!(
        matches!(line, Some(Err(_))),
        "the lines are cut, not {line:?}"
    );
    assert!(
        matches!(chunk, Some(Err(_))),
        "the bytes are cut, not {chunk:?}"
    );

    let (served, log) = done.await.expect("the server's thread does not panic");

    served.expect("the server stops with no error");

    let log = log.text();
    let warned: Vec<&str> = log.lines().filter(|line| line.contains(" WARN ")).collect();

    assert_eq!(warned.len(), 1, "{log}");
    assert!(warned[0].contains("the grace ran out"), "{log}");
    Ok(())
}
