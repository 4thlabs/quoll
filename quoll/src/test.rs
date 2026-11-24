//! Support for an application's tests: its router served on a free port, so that a test calls it over HTTP with the
//! real client.

use axum::Router;
use axum::serve::{Listener, ListenerExt};
use tokio::net::TcpListener;

use crate::server::no_delay::no_delay;

/// Serves `router` on a free port of 127.0.0.1, and gives its base URL.
///
/// The URL is `http://127.0.0.1:<port>`, with no final slash: what `Api::client` and `client::builder()`'s
/// `build::<Api>` take. A router with the layers of a [`server::Builder`](crate::server::Builder) is the one its
/// [`into_router`](crate::server::Builder::into_router) gives.
///
/// Its connections have `TCP_NODELAY`, as those of [`serve`](crate::server::Builder::serve) do, so that a stream's
/// small chunks reach the test at once.
///
/// The server runs on a task of the tokio runtime it is called from, and lives as long as that runtime: to the end of
/// the test, as `#[tokio::test]` gives each test a runtime of its own, which stops the server when the test returns.
///
/// ```
/// use quoll::test;
///
/// # #[tokio::main(flavor = "current_thread")]
/// # async fn main() -> Result<(), reqwest::Error> {
/// let router = axum::Router::new().route("/health", axum::routing::get(|| async { "ok" }));
/// let url = test::spawn(router).await;
///
/// let answer = reqwest::get(format!("{url}/health")).await?.text().await?;
///
/// assert_eq!(answer, "ok");
/// # Ok(())
/// # }
/// ```
///
/// # Panics
///
/// When no port of 127.0.0.1 can be bound, or when it is called outside a tokio runtime.
pub async fn spawn(router: Router) -> String {
    let listener = TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port of 127.0.0.1 is free");
    let address = listener
        .local_addr()
        .expect("a bound listener has an address");

    serve(listener, router);

    format!("http://{address}")
}

/// Serves `router` on `listener`, on a task of the runtime.
fn serve<L>(listener: L, router: Router)
where
    L: Listener,
    L::Addr: std::fmt::Debug,
{
    // axum's server never ends: it waits and accepts again when accepting a connection fails, so its result, which
    // only `Ok` would be, is not read.
    tokio::spawn(async move { axum::serve(listener.tap_io(no_delay), router).await });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::server::no_delay::Watched;

    #[tokio::test]
    async fn a_connection_of_the_test_server_has_tcp_nodelay() {
        let router = Router::new().route("/", axum::routing::get(|| async { "ok" }));
        let (listener, address, mut connections) = Watched::bind().await;

        serve(listener, router);

        // The connection's options are set before its first request is read: once answered, they hold.
        let answer = reqwest::get(format!("http://{address}/"))
            .await
            .expect("the server answers");
        let connection = connections
            .recv()
            .await
            .expect("the server accepted a connection");

        assert_eq!(answer.text().await.expect("the body reads"), "ok");
        assert!(
            connection
                .nodelay()
                .expect("the socket's option can be read"),
            "the accepted connection has TCP_NODELAY"
        );
    }
}
