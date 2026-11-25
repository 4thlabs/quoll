//! A bound on how long a write of the server waits for its client to take bytes, on the io of each connection, as
//! nginx's `send_timeout`: past it, the write fails, hyper closes the connection and drops the answer's body, and so
//! the handler's stream, as when the client disconnects.

use std::any::Any;
use std::fmt;
use std::future::Future;
use std::io;
use std::pin::Pin;
use std::task::{Context, Poll, ready};
use std::time::Duration;

use axum::serve::Listener;
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};
use tokio::net::TcpStream;
use tokio::time::{Instant, Sleep};

/// The bound unless it is set: nginx's `send_timeout`.
pub(crate) const SEND_TIMEOUT: Duration = Duration::from_secs(60);

/// A listener whose connections are [`SendTimeout`]s, bounded by `bound`, or left as they are with none.
pub(crate) struct SendTimeouts<L> {
    listener: L,
    bound: Option<Duration>,
}

/// `listener`, each of its connections bounded by `bound` when there is one.
pub(crate) fn send_timeouts<L>(listener: L, bound: Option<Duration>) -> SendTimeouts<L> {
    SendTimeouts { listener, bound }
}

impl<L> Listener for SendTimeouts<L>
where
    L: Listener,
    L::Addr: fmt::Debug,
{
    type Io = SendTimeout<L::Io>;
    type Addr = L::Addr;

    async fn accept(&mut self) -> (Self::Io, Self::Addr) {
        let (io, address) = self.listener.accept().await;
        let peer = format!("{address:?}");

        (SendTimeout::new(io, self.bound, peer), address)
    }

    fn local_addr(&self) -> io::Result<Self::Addr> {
        self.listener.local_addr()
    }
}

/// The io of a connection, whose writes wait at most `bound` for the client to take bytes.
///
/// The clock runs only while a write waits: it starts when the io refuses bytes, and stops when it takes any. A reader
/// that takes some bytes within each `bound`, however few, is never cut. A connection with nothing to send, a quiet
/// stream between two events, runs no clock. A write takes no allocation and no timer: the first refusal allocates
/// the deadline, which later ones reset.
pub(crate) struct SendTimeout<Io> {
    io: Io,
    bound: Option<Duration>,
    /// The deadline of the write that waits, armed at the first refusal.
    deadline: Option<Pin<Box<Sleep>>>,
    is_waiting: bool,
    /// The bound that cut the connection, once it has passed: every write fails from then on, and the cut is logged
    /// once.
    cut: Option<Duration>,
    /// The client's address, as the listener gives it, for the log of the cut.
    peer: String,
}

impl<Io: Any> SendTimeout<Io> {
    fn new(io: Io, bound: Option<Duration>, peer: String) -> Self {
        Self {
            io,
            bound,
            deadline: None,
            is_waiting: false,
            cut: None,
            peer,
        }
    }

    /// What a write the io refused does: it waits, until the deadline armed at the first refusal, then fails.
    fn waited<T>(&mut self, cx: &mut Context<'_>) -> Poll<io::Result<T>> {
        let Some(bound) = self.bound else {
            return Poll::Pending;
        };

        if !self.is_waiting {
            // A bound too large for the clock, as `Duration::MAX`, is no bound.
            let Some(at) = Instant::now().checked_add(bound) else {
                return Poll::Pending;
            };

            self.is_waiting = true;

            match &mut self.deadline {
                Some(deadline) => deadline.as_mut().reset(at),
                None => self.deadline = Some(Box::pin(tokio::time::sleep_until(at))),
            }
        }

        let Some(deadline) = &mut self.deadline else {
            return Poll::Pending;
        };

        ready!(deadline.as_mut().poll(cx));

        self.cut(bound);

        Poll::Ready(Err(timed_out(bound)))
    }

    /// Cuts the connection once its bound has passed: a reset on tokio's `TcpStream`, so that the kernel drops at once
    /// what it holds for the client rather than keep it until the client reads it or the kernel gives up, as nginx's
    /// `reset_timedout_connection`. Any other io is closed when hyper drops it.
    fn cut(&mut self, bound: Duration) {
        self.cut = Some(bound);

        if let Some(tcp) = (&mut self.io as &mut dyn Any).downcast_mut::<TcpStream>()
            && let Err(error) = tcp.set_zero_linger()
        {
            tracing::debug!(%error, "SO_LINGER could not be set: the connection is closed rather than reset");
        }

        tracing::info!(
            peer = %self.peer,
            ?bound,
            "a connection was closed: its client took no bytes of the answer for {bound:?}"
        );
    }

    /// What a write the io answered does: it stops the clock.
    fn answered<T>(&mut self, answer: io::Result<T>) -> Poll<io::Result<T>> {
        self.is_waiting = false;

        Poll::Ready(answer)
    }

    /// A write through `write`, bounded: it fails at once once the connection is cut.
    fn write<T>(
        &mut self,
        cx: &mut Context<'_>,
        write: impl FnOnce(Pin<&mut Io>, &mut Context<'_>) -> Poll<io::Result<T>>,
    ) -> Poll<io::Result<T>>
    where
        Io: Unpin,
    {
        if let Some(bound) = self.cut {
            return Poll::Ready(Err(timed_out(bound)));
        }

        match write(Pin::new(&mut self.io), cx) {
            Poll::Ready(answer) => self.answered(answer),
            Poll::Pending => self.waited(cx),
        }
    }
}

/// The error of a write cut by its bound.
fn timed_out(bound: Duration) -> io::Error {
    io::Error::new(
        io::ErrorKind::TimedOut,
        format!("the client took no bytes for {bound:?}"),
    )
}

impl<Io: AsyncRead + Unpin> AsyncRead for SendTimeout<Io> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Pin::new(&mut self.get_mut().io).poll_read(cx, buf)
    }
}

impl<Io: AsyncWrite + Unpin + Any> AsyncWrite for SendTimeout<Io> {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        self.get_mut().write(cx, |io, cx| io.poll_write(cx, buf))
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[io::IoSlice<'_>],
    ) -> Poll<io::Result<usize>> {
        self.get_mut()
            .write(cx, |io, cx| io.poll_write_vectored(cx, bufs))
    }

    fn is_write_vectored(&self) -> bool {
        self.io.is_write_vectored()
    }

    // A TLS stream flushes what it holds, and its shutdown writes its `close_notify`: each waits for the client as a
    // write does.
    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.get_mut().write(cx, |io, cx| io.poll_flush(cx))
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.get_mut().write(cx, |io, cx| io.poll_shutdown(cx))
    }
}

#[cfg(test)]
mod tests {
    use std::io::ErrorKind;

    use tokio::io::{AsyncReadExt, AsyncWriteExt, DuplexStream};

    use super::*;

    /// A pipe of 64 bytes: a write past them waits for the other end to read, as a full socket does.
    fn pipe(bound: Option<Duration>) -> (SendTimeout<DuplexStream>, DuplexStream) {
        let (ours, theirs) = tokio::io::duplex(64);

        (SendTimeout::new(ours, bound, "test".to_owned()), theirs)
    }

    /// An io whose writes, flushes and shutdowns always wait, as one whose client takes nothing.
    struct Stalled;

    impl AsyncWrite for Stalled {
        fn poll_write(
            self: Pin<&mut Self>,
            _: &mut Context<'_>,
            _: &[u8],
        ) -> Poll<io::Result<usize>> {
            Poll::Pending
        }

        fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Pending
        }

        fn poll_shutdown(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
            Poll::Pending
        }
    }

    #[tokio::test(start_paused = true)]
    async fn a_write_that_waits_past_the_bound_fails() {
        let (mut ours, _theirs) = pipe(Some(SEND_TIMEOUT));
        let started = Instant::now();
        let error = ours.write_all(&[0; 128]).await.expect_err("nobody reads");

        assert_eq!(error.kind(), ErrorKind::TimedOut);
        assert_eq!(error.to_string(), "the client took no bytes for 60s");
        assert_eq!(started.elapsed(), SEND_TIMEOUT);
    }

    #[tokio::test(start_paused = true)]
    async fn a_reader_that_takes_bytes_within_the_bound_keeps_it() {
        let (mut ours, mut theirs) = pipe(Some(SEND_TIMEOUT));
        let reading = async move {
            let mut buffer = [0; 16];

            // 16 bytes every 50 s: far slower than a byte a second, yet no write waits past the bound.
            for _ in 0..64 {
                tokio::time::sleep(Duration::from_secs(50)).await;
                theirs.read_exact(&mut buffer).await.expect("a read");
            }
        };
        // The write ends with its half of the pipe, so that a reader cut short reads the end rather than waiting.
        let writing = async move { ours.write_all(&[0; 1024]).await };
        let (written, ()) = tokio::join!(writing, reading);

        written.expect("a reader within the bound is kept");
    }

    #[tokio::test(start_paused = true)]
    async fn the_clock_runs_only_while_a_write_waits() {
        let (mut ours, mut theirs) = pipe(Some(SEND_TIMEOUT));

        ours.write_all(&[0; 64])
            .await
            .expect("the pipe takes 64 bytes");

        // An hour with nothing to write: no clock runs.
        tokio::time::sleep(Duration::from_secs(3600)).await;

        let mut buffer = [0; 64];

        theirs.read_exact(&mut buffer).await.expect("a read");
        ours.write_all(&[0; 64])
            .await
            .expect("the pipe takes 64 more");
    }

    #[tokio::test(start_paused = true)]
    async fn with_no_bound_a_write_waits_for_ever() {
        let (mut ours, _theirs) = pipe(None);
        let writing = ours.write_all(&[0; 128]);

        assert!(
            tokio::time::timeout(Duration::from_secs(24 * 3600), writing)
                .await
                .is_err(),
            "the write still waits after a day"
        );
    }

    #[tokio::test(start_paused = true)]
    async fn a_bound_too_large_for_the_clock_is_no_bound() {
        let (mut ours, _theirs) = pipe(Some(Duration::MAX));
        let writing = ours.write_all(&[0; 128]);

        assert!(
            tokio::time::timeout(Duration::from_secs(24 * 3600), writing)
                .await
                .is_err(),
            "the write still waits after a day"
        );
    }

    #[tokio::test(start_paused = true)]
    async fn a_flush_and_a_shutdown_are_bounded_as_a_write() {
        let mut flushed = SendTimeout::new(Stalled, Some(SEND_TIMEOUT), "test".to_owned());
        let mut shut = SendTimeout::new(Stalled, Some(SEND_TIMEOUT), "test".to_owned());
        let started = Instant::now();

        let flush = flushed.flush().await.expect_err("nobody reads");
        let shutdown = shut.shutdown().await.expect_err("nobody reads");

        assert_eq!(flush.kind(), ErrorKind::TimedOut);
        assert_eq!(shutdown.kind(), ErrorKind::TimedOut);
        assert_eq!(started.elapsed(), 2 * SEND_TIMEOUT);
    }

    #[tokio::test(start_paused = true)]
    async fn once_cut_every_write_fails_at_once() {
        let mut cut = SendTimeout::new(Stalled, Some(SEND_TIMEOUT), "test".to_owned());

        cut.write_all(b"x").await.expect_err("nobody reads");

        let cut_at = Instant::now();
        let write = cut
            .write_all(b"x")
            .await
            .expect_err("the connection is cut");
        let shutdown = cut.shutdown().await.expect_err("the connection is cut");

        assert_eq!(write.kind(), ErrorKind::TimedOut);
        assert_eq!(shutdown.kind(), ErrorKind::TimedOut);
        assert_eq!(cut_at.elapsed(), Duration::ZERO);
    }
}
