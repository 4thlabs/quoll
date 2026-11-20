//! Support for an application's tests: its router served on a free port, so that a test calls it over HTTP with the
//! real client.

use axum::Router;
use tokio::net::TcpListener;

/// Serves `router` on a free port of 127.0.0.1, and gives its base URL, `http://127.0.0.1:<port>`, with no final
/// slash: what `Api::client` and `client::Builder::build` take. A router with the layers of a `server::Builder` is the
/// one its `into_router` gives.
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

    // axum's server never ends: it waits and accepts again when accepting a connection fails, so its result, which
    // only `Ok` would be, is not read.
    tokio::spawn(async move { axum::serve(listener, router).await });

    format!("http://{address}")
}
