//! The result of a route that switches its connection to `tcp`: the bytes both ways, once HTTP is over.

use std::borrow::Cow;
use std::fmt;
#[cfg(feature = "client")]
use std::pin::Pin;

#[cfg(feature = "client")]
use sync_wrapper::SyncWrapper;

use crate::__private::content_type;

#[cfg(feature = "client")]
mod io;

/// A connection that a `101 Switching Protocols` switched to `tcp`: what the client gives for a route
/// `-> TcpTunnel`, the bytes both ways, as they come.
///
/// The request of such a route always sends `Connection: Upgrade` and `Upgrade: tcp`, as the Docker Engine API and
/// Podman expect for `exec` and `attach`, and the client's middlewares run on it as on any call. Once the other api
/// answers `101` with `Upgrade: tcp`, HTTP is over: the connection is the tunnel, whether it is TCP or a Unix socket.
/// It reads with tokio's [`AsyncRead`] and writes with its [`AsyncWrite`], so that `tokio::io::split`,
/// `tokio::io::copy` and `copy_bidirectional` take it. [`shutdown`] closes the half it writes, which tells the other end that nothing more comes, and the half it reads stays open; dropping the
/// tunnel closes the connection. After the 101, a failure is a `std::io::Error` of a read or a write, not a
/// [`quoll::Error`](crate::Error): the tunnel is a pipe of bytes, with no deadline.
///
/// A route `-> TcpTunnel` compiles only in a contract with `#[server(none)]`: quoll's server does not open a tunnel
/// yet.
///
/// ```
/// use quoll::TcpTunnel;
///
/// quoll::api! {
///     #[server(none)]
///     pub api Docker = "/v1.47";
///
///     pub group Exec {
///         POST "/exec/{id}/start" => start(id: &str) -> TcpTunnel;
///     }
/// }
/// ```
///
/// The type exists whatever the features, so that a contract crate built with neither names it; its connection, its
/// constructors and tokio's traits come with the feature `client`.
///
#[cfg_attr(
    feature = "client",
    doc = "[`AsyncRead`]: tokio::io::AsyncRead\n[`AsyncWrite`]: tokio::io::AsyncWrite\n[`shutdown`]: tokio::io::AsyncWriteExt::shutdown"
)]
#[cfg_attr(
    not(feature = "client"),
    doc = "[`AsyncRead`]: crate#features\n[`AsyncWrite`]: crate#features\n[`shutdown`]: crate#features"
)]
pub struct TcpTunnel {
    #[cfg(feature = "client")]
    io: SyncWrapper<Pin<Box<dyn io::Io>>>,
    content_type: Cow<'static, str>,
}

impl TcpTunnel {
    /// What the bytes are, as `Content-Type` writes it: the content type of the 101, or the one a fake gave,
    /// `application/octet-stream` when nothing said it. The Docker Engine API sends
    /// `application/vnd.docker.raw-stream` for a TTY's bytes as they are, and
    /// `application/vnd.docker.multiplexed-stream` for its frames.
    pub fn content_type(&self) -> &str {
        &self.content_type
    }

    /// Whether the bytes are of `media_type`, a type and a subtype, whatever the case and the parameters of their
    /// content type, as [`ByteStream::has_type`](crate::ByteStream::has_type) reads it.
    pub fn has_type(&self, media_type: &str) -> bool {
        content_type::essence(&self.content_type).eq_ignore_ascii_case(media_type.trim())
    }
}

/// Shows the content type: the bytes are known only as they are read.
impl fmt::Debug for TcpTunnel {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TcpTunnel")
            .field("content_type", &self.content_type)
            .finish_non_exhaustive()
    }
}
