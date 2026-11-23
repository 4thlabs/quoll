//! What a client's error shows of the URL of its call: its origin and its path, never its query, where a query scheme
//! sends its key. Each call fails against a server of raw TCP that closes its connections with no answer, or answers
//! what no server of a contract would.

#![cfg(feature = "client")]

use std::error::Error as _;
use std::future::Future;
use std::pin::Pin;
use std::sync::{Arc, Mutex};

use http::Extensions;
use quoll::{Error, client::Attempt};
use reqwest_middleware::{ClientBuilder, ClientWithMiddleware, Middleware, Next};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

quoll::api! {
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
    let http: ClientWithMiddleware = http.with(quoll::client::key::<Token>(KEY)).build();

    Api::client_with(http, url)
        .expect("the url is the api's")
        .keyed()
        .keyed()
        .await
        .expect_err("the call fails")
}

/// A URL where nothing answers: a port kept bound, whose every connection is closed at once, with no response. A port
/// freed instead could be taken by another test, which would answer.
async fn dead_url() -> String {
    answers_once(None).await
}

/// A server of raw TCP that reads the head of the first request and writes `answer` on its connection, then closes
/// it, and closes each later connection at once, as it closes the first when there is no `answer`: its URL.
async fn answers_once(answer: Option<&'static [u8]>) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("a free port");
    let url = format!("http://{}", listener.local_addr().expect("an address"));

    tokio::spawn(async move {
        if let Some(answer) = answer {
            let (connection, _) = listener.accept().await.expect("a connection");

            answer_once(connection, answer).await;
        }

        while let Ok((connection, _)) = listener.accept().await {
            drop(connection);
        }
    });

    url
}

/// Reads the head of a request on `connection`, then writes `answer` and closes it.
async fn answer_once(mut connection: TcpStream, answer: &[u8]) {
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
async fn a_call_that_got_no_response_shows_no_key() {
    let url = dead_url().await;

    let error = call(ClientBuilder::new(reqwest::Client::new()), &url).await;

    assert_shows_no_key(&error, "/api/keyed");
}

#[tokio::test]
async fn a_failure_tried_again_shows_no_key() {
    let url = dead_url().await;
    let http = ClientBuilder::new(reqwest::Client::new()).with(quoll::client::retry(1));

    let error = call(http, &url).await;

    assert_shows_no_key(&error, "/api/keyed");
}

#[tokio::test]
async fn an_attempt_retry_decides_on_shows_no_key() {
    let url = dead_url().await;
    let shown: Arc<Mutex<Vec<String>>> = Arc::default();
    let retry = {
        let shown = Arc::clone(&shown);

        quoll::client::retry(1).when(move |attempt: &Attempt| {
            let error = attempt.error().expect("no answer");

            shown
                .lock()
                .expect("no test panics while holding it")
                .push(format!("{error}\n{error:?}\n{attempt:?}"));
            attempt.transient()
        })
    };

    let error = call(ClientBuilder::new(reqwest::Client::new()).with(retry), &url).await;

    assert_shows_no_key(&error, "/api/keyed");

    let shown = shown.lock().expect("no test panics while holding it");

    assert_eq!(shown.len(), 2);
    for shown in shown.iter() {
        assert!(!shown.contains(KEY), "{shown}");
        assert!(shown.contains("/api/keyed"), "{shown}");
    }
}

#[tokio::test]
async fn a_failure_retry_does_not_try_again_shows_no_key() {
    let url = dead_url().await;
    let http = ClientBuilder::new(reqwest::Client::new()).with(quoll::client::retry(0));

    let error = call(http, &url).await;

    assert_shows_no_key(&error, "/api/keyed");
}

#[tokio::test]
async fn a_failure_in_the_error_of_a_middleware_shows_no_key() {
    let url = dead_url().await;

    for whole in [false, true] {
        let http = ClientBuilder::new(reqwest::Client::new()).with(wrapping(whole));

        let error = call(http, &url).await;

        assert_shows_no_key(&error, "/api/keyed");
    }
}

#[tokio::test]
async fn a_hop_of_redirects_that_got_no_response_shows_no_key() {
    let url = answers_once(Some(
        b"HTTP/1.1 302 Found\r\nLocation: /api/moved\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    ))
    .await;
    let http = reqwest::Client::builder()
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .expect("a client");
    let http = ClientBuilder::new(http).with(quoll::client::redirects(1));

    let error = call(http, &url).await;

    assert_shows_no_key(&error, "/api/moved");
}

#[tokio::test]
async fn a_body_cut_short_shows_no_key() {
    let url = answers_once(Some(
        b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n\"cut",
    ))
    .await;

    let error = call(ClientBuilder::new(reqwest::Client::new()), &url).await;

    assert_shows_no_key(&error, "/api/keyed");
}

#[tokio::test]
async fn an_internal_error_logs_no_key_of_a_reqwest_error() {
    let url = format!("{}/api/keyed?token={KEY}", dead_url().await);
    let direct = reqwest::get(&url).await.expect_err("no response");
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::retry(0))
        .build();
    let chained = http.get(&url).send().await.expect_err("no response");

    for error in [Error::internal(direct), Error::internal(chained)] {
        let shown = shown(&error);

        assert!(!shown.contains(KEY), "{shown}");
        assert!(shown.contains("/api/keyed"), "{shown}");
    }
}

#[tokio::test]
async fn retry_leaves_the_url_of_a_request_of_no_contract_whole() {
    let url = format!("{}/feed?page=2", dead_url().await);
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::retry(0))
        .build();

    let error = http.get(&url).send().await.expect_err("no response");

    assert_eq!(error.url().map(reqwest::Url::as_str), Some(url.as_str()));
}
