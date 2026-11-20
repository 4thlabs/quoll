//! What a client's error shows of the URL of its call: its origin and its path, never its query, where a query scheme
//! sends its key. Each call fails against a port nothing listens on, or a server of raw TCP that answers what no
//! server of a contract would.

#![cfg(feature = "client")]

use std::error::Error as _;
use std::future::Future;
use std::pin::Pin;

use http::Extensions;
use quoll::rest::{self, Error};
use reqwest_middleware::{ClientBuilder, ClientWithMiddleware, Middleware, Next};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

rest::api! {
    api Api = "/api";

    /// A key in the query, which a URL shows.
    scheme Token = query "token";

    #[auth(Token)]
    group Keyed {
        GET "/keyed" => keyed() -> String;
    }
}

/// The key each call sends in its query, which no error may show.
const KEY: &str = "k-secret-1";

/// The text of `error` in every form a log writes it: its message and its debug form, and those of each error it
/// comes from.
fn shown(error: &Error) -> String {
    let mut shown = vec![error.to_string(), format!("{error:?}")];
    let mut source = error.source();

    while let Some(error) = source {
        shown.push(error.to_string());
        shown.push(format!("{error:?}"));
        source = error.source();
    }

    shown.join("\n")
}

/// Asserts that `error` shows `path`, where the call went, and no key.
fn assert_shows_no_key(error: &Error, path: &str) {
    let shown = shown(error);

    assert_eq!(error.status(), None, "{shown}");
    assert!(!shown.contains(KEY), "{shown}");
    assert!(shown.contains(path), "{path} in {shown}");
}

/// Calls the route through `http`, with the key added last, and returns its error.
async fn call(http: ClientBuilder, url: &str) -> Error {
    let http: ClientWithMiddleware = http.with(rest::client::key::<Token>(KEY)).build();

    Api::client_with(http, url)
        .expect("the url is the api's")
        .keyed()
        .keyed()
        .await
        .expect_err("the call fails")
}

/// The URL of a port nothing listens on, where each connection is refused.
async fn refused() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("a free port");

    format!("http://{}", listener.local_addr().expect("an address"))
}

/// A server of raw TCP that writes `answer` on the first connection it accepts, then closes it, and closes each
/// later one at once: its URL.
async fn answers_once(answer: &'static [u8]) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("a free port");
    let url = format!("http://{}", listener.local_addr().expect("an address"));

    tokio::spawn(async move {
        let (mut connection, _) = listener.accept().await.expect("a connection");
        let mut head = Vec::new();
        let mut buffer = [0; 1024];

        while !head.windows(4).any(|window| window == b"\r\n\r\n") {
            let read = connection.read(&mut buffer).await.expect("a request");

            if read == 0 {
                return;
            }
            head.extend_from_slice(&buffer[..read]);
        }
        connection.write_all(answer).await.expect("an answer");
        drop(connection);

        loop {
            let (connection, _) = listener.accept().await.expect("a connection");

            drop(connection);
        }
    });

    url
}

/// A middleware of the application's own that fails a call with the error of the rest of the chain inside its own,
/// as `reqwest_middleware::Error::middleware` wraps it: reqwest's error alone, or the whole error of the chain.
fn wrapping(whole: bool) -> impl Middleware {
    middleware(move |request, extensions, next| {
        Box::pin(async move {
            next.run(request, extensions)
                .await
                .map_err(|error| match error {
                    reqwest_middleware::Error::Reqwest(error) if !whole => {
                        reqwest_middleware::Error::middleware(error)
                    }
                    error => reqwest_middleware::Error::middleware(error),
                })
        })
    })
}

/// What a middleware's call returns.
type Handled<'a> =
    Pin<Box<dyn Future<Output = reqwest_middleware::Result<reqwest::Response>> + Send + 'a>>;

/// Gives a closure the signature reqwest-middleware takes, for every lifetime of its request.
fn middleware<F>(handle: F) -> F
where
    F: for<'a> Fn(reqwest::Request, &'a mut Extensions, Next<'a>) -> Handled<'a>
        + Send
        + Sync
        + 'static,
{
    handle
}

#[tokio::test]
async fn a_refused_connection_shows_no_key() {
    let url = refused().await;

    let error = call(ClientBuilder::new(reqwest::Client::new()), &url).await;

    assert_shows_no_key(&error, "/api/keyed");
}

#[tokio::test]
async fn a_refused_connection_tried_again_shows_no_key() {
    let url = refused().await;
    let http = ClientBuilder::new(reqwest::Client::new()).with(rest::client::retry(1));

    let error = call(http, &url).await;

    assert_shows_no_key(&error, "/api/keyed");
}

#[tokio::test]
async fn a_refused_connection_in_the_error_of_a_middleware_shows_no_key() {
    let url = refused().await;

    for whole in [false, true] {
        let http = ClientBuilder::new(reqwest::Client::new()).with(wrapping(whole));

        let error = call(http, &url).await;

        assert_shows_no_key(&error, "/api/keyed");
    }
}

#[tokio::test]
async fn a_hop_of_redirects_that_got_no_response_shows_no_key() {
    let url = answers_once(
        b"HTTP/1.1 302 Found\r\nLocation: /api/moved\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    )
    .await;
    let http = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .expect("a client");
    let http = ClientBuilder::new(http).with(rest::client::redirects(1));

    let error = call(http, &url).await;

    assert_shows_no_key(&error, "/api/moved");
}

#[tokio::test]
async fn a_body_cut_short_shows_no_key() {
    let url = answers_once(
        b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n\"cut",
    )
    .await;

    let error = call(ClientBuilder::new(reqwest::Client::new()), &url).await;

    assert_shows_no_key(&error, "/api/keyed");
}
