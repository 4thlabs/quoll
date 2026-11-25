//! The connection of a [`TcpTunnel`], with the feature `client`: its constructors, and tokio's `AsyncRead` and
//! `AsyncWrite`.

use std::borrow::Cow;
use std::io;
use std::pin::Pin;
use std::task::{Context, Poll};

use sync_wrapper::SyncWrapper;
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};

use super::TcpTunnel;
use crate::__private::content_type::OCTET_STREAM;

/// What the connection of a tunnel may be: any stream of bytes both ways.
pub(super) trait Io: AsyncRead + AsyncWrite + Send {}

impl<T: AsyncRead + AsyncWrite + Send> Io for T {}

impl TcpTunnel {
    /// A tunnel on `io`, of `application/octet-stream`, bytes of no known type: what a fake of the client returns,
    /// the half of a `tokio::io::duplex` whose other half a task plays the other api on.
    ///
    /// ```
    /// use quoll::TcpTunnel;
    /// use tokio::io::{AsyncReadExt, AsyncWriteExt};
    ///
    /// # #[tokio::main(flavor = "current_thread")]
    /// # async fn main() -> std::io::Result<()> {
    /// let (ours, mut theirs) = tokio::io::duplex(1024);
    /// let mut tunnel = TcpTunnel::new(ours);
    ///
    /// tunnel.write_all(b"ping").await?;
    ///
    /// let mut received = [0; 4];
    /// theirs.read_exact(&mut received).await?;
    ///
    /// assert_eq!(&received, b"ping");
    /// # Ok(())
    /// # }
    /// ```
    pub fn new(io: impl AsyncRead + AsyncWrite + Send + 'static) -> Self {
        Self {
            io: SyncWrapper::new(Box::pin(io)),
            content_type: Cow::Borrowed(OCTET_STREAM),
        }
    }

    /// The same tunnel, of `content_type`, a media type as `Content-Type` writes it: what a fake says of its bytes,
    /// as `application/vnd.docker.raw-stream`.
    #[must_use]
    pub fn with_content_type(self, content_type: impl Into<Cow<'static, str>>) -> Self {
        Self {
            content_type: content_type.into(),
            ..self
        }
    }

    /// The tunnel of a connection reqwest gave back, of the content type of its 101.
    pub(crate) fn upgraded(upgraded: reqwest::Upgraded, content_type: Cow<'static, str>) -> Self {
        Self::new(upgraded).with_content_type(content_type)
    }
}

/// The bytes the other end writes, as they come; the end of them once it has closed its half.
impl AsyncRead for TcpTunnel {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        self.get_mut().io.get_mut().as_mut().poll_read(cx, buf)
    }
}

/// The bytes sent to the other end. `poll_shutdown` closes the half the tunnel writes, and the other stays open.
impl AsyncWrite for TcpTunnel {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        self.get_mut().io.get_mut().as_mut().poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.get_mut().io.get_mut().as_mut().poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        self.get_mut().io.get_mut().as_mut().poll_shutdown(cx)
    }
}

#[cfg(test)]
mod tests {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    use super::*;

    fn is_send_sync_and_unpin<T: Send + Sync + Unpin>(_: &T) {}

    #[test]
    fn a_tunnel_is_send_sync_and_unpin_whatever_its_connection() {
        // A connection that is `Send` and not `Sync`, as a `TokioIo` of hyper's may be.
        struct NotSync(
            std::marker::PhantomData<std::cell::Cell<u8>>,
            tokio::io::DuplexStream,
        );

        impl AsyncRead for NotSync {
            fn poll_read(
                mut self: Pin<&mut Self>,
                cx: &mut Context<'_>,
                buf: &mut ReadBuf<'_>,
            ) -> Poll<io::Result<()>> {
                Pin::new(&mut self.1).poll_read(cx, buf)
            }
        }

        impl AsyncWrite for NotSync {
            fn poll_write(
                mut self: Pin<&mut Self>,
                cx: &mut Context<'_>,
                buf: &[u8],
            ) -> Poll<io::Result<usize>> {
                Pin::new(&mut self.1).poll_write(cx, buf)
            }

            fn poll_flush(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
                Pin::new(&mut self.1).poll_flush(cx)
            }

            fn poll_shutdown(
                mut self: Pin<&mut Self>,
                cx: &mut Context<'_>,
            ) -> Poll<io::Result<()>> {
                Pin::new(&mut self.1).poll_shutdown(cx)
            }
        }

        let (ours, _theirs) = tokio::io::duplex(8);
        let tunnel = TcpTunnel::new(NotSync(std::marker::PhantomData, ours));

        is_send_sync_and_unpin(&tunnel);
        assert_eq!(tunnel.content_type(), OCTET_STREAM);
    }

    #[tokio::test]
    async fn a_shutdown_closes_the_half_it_writes_and_reads_on() -> io::Result<()> {
        let (ours, mut theirs) = tokio::io::duplex(64);
        let mut tunnel =
            TcpTunnel::new(ours).with_content_type("application/vnd.docker.raw-stream");

        tunnel.write_all(b"hello").await?;
        tunnel.shutdown().await?;

        let mut received = Vec::new();
        theirs.read_to_end(&mut received).await?;
        assert_eq!(received, b"hello");

        theirs.write_all(b"bye").await?;
        drop(theirs);

        let mut answered = Vec::new();
        tunnel.read_to_end(&mut answered).await?;
        assert_eq!(answered, b"bye");
        assert!(tunnel.has_type("Application/Vnd.Docker.Raw-Stream"));
        Ok(())
    }

    #[tokio::test]
    async fn a_dropped_tunnel_closes_its_connection() -> io::Result<()> {
        let (ours, mut theirs) = tokio::io::duplex(64);

        drop(TcpTunnel::new(ours));

        let mut received = Vec::new();
        theirs.read_to_end(&mut received).await?;
        assert!(received.is_empty());
        Ok(())
    }
}
