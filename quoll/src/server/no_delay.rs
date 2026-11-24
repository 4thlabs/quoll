//! `TCP_NODELAY` on the connections a server accepts, with the listener its tests watch them through.

use std::any::Any;

use tokio::net::TcpStream;

/// Turns Nagle's algorithm off on a connection whose stream is tokio's `TcpStream`, and leaves any other alone: a
/// stream that wraps one, TLS or another wrapper, is not seen through. The server sends each chunk a handler gives
/// as soon as it is given, an event or a few bytes of a `ByteStream`, so its latency matters more than coalescing:
/// with the client's delayed ACK, Nagle holds a small write for about 40 ms, each chunk of a stream paying it again.
pub(crate) fn no_delay<Io: Any>(io: &mut Io) {
    let Some(tcp) = (io as &mut dyn Any).downcast_mut::<TcpStream>() else {
        return;
    };

    if let Err(error) = tcp.set_nodelay(true) {
        tracing::debug!(%error, "TCP_NODELAY could not be set: the connection is served with Nagle's algorithm");
    }
}

#[cfg(test)]
pub(crate) use watched::Watched;

/// The listener the tests of a server watch its connections through.
#[cfg(test)]
mod watched {
    use std::io;
    use std::net::SocketAddr;

    use axum::serve::Listener;
    use tokio::net::{TcpListener, TcpStream};
    use tokio::sync::mpsc;

    /// A listener on a free port of 127.0.0.1 that sends a copy of each connection it accepts, the same socket, for a
    /// test to read its options.
    pub(crate) struct Watched {
        listener: TcpListener,
        accepted: mpsc::UnboundedSender<std::net::TcpStream>,
    }

    impl Watched {
        /// The listener, its address, and the copies of the connections it accepts.
        pub(crate) async fn bind() -> (
            Self,
            SocketAddr,
            mpsc::UnboundedReceiver<std::net::TcpStream>,
        ) {
            let listener = TcpListener::bind("127.0.0.1:0")
                .await
                .expect("a port of 127.0.0.1 is free");
            let address = listener
                .local_addr()
                .expect("a bound listener has an address");
            let (accepted, connections) = mpsc::unbounded_channel();

            (Self { listener, accepted }, address, connections)
        }
    }

    impl Listener for Watched {
        type Io = TcpStream;
        type Addr = SocketAddr;

        async fn accept(&mut self) -> (TcpStream, SocketAddr) {
            // A failed accept, as a connection aborted before it is accepted, is tried again, as axum's listener does.
            let (stream, address) = loop {
                if let Ok(accepted) = self.listener.accept().await {
                    break accepted;
                }
            };
            let stream = stream
                .into_std()
                .expect("an accepted stream leaves the runtime");
            let copy = stream
                .try_clone()
                .expect("an accepted stream can be duplicated");

            // The test may have stopped reading: its copy is then of no use.
            let _ = self.accepted.send(copy);

            let stream = TcpStream::from_std(stream).expect("the stream goes back to the runtime");

            (stream, address)
        }

        fn local_addr(&self) -> io::Result<SocketAddr> {
            self.listener.local_addr()
        }
    }
}
