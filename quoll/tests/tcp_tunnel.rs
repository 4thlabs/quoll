//! The result `TcpTunnel`: a route whose 101 switches its connection to `tcp`, called on a daemon written in axum
//! without a contract, on port 0 and on a Unix socket, through every helper of the client; the strict check of the
//! 101, the errors of each kind, and a fake on `tokio::io::duplex`.

#![cfg(feature = "client")]

use std::fmt;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::Router;
use axum::body::Body;
use axum::extract::{Path, Request};
use axum::response::{IntoResponse, Response};
use axum::routing::post;
use http::{Extensions, HeaderMap, HeaderValue, StatusCode, header};
use hyper_util::rt::TokioIo;
use quoll::{Call, ErrorKind, TcpTunnel, client};
use reqwest_middleware::{Middleware, Next};
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::Notify;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
struct Start {
    tty: bool,
}

/// The daemon's own error shape, not an RFC 9457 problem.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
struct Refused {
    message: String,
}

impl fmt::Display for Refused {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

quoll::api! {
    #[server(none)]
    #[error(Refused)]
    api Daemon = "/v1";

    #[auth(Bearer)]
    group Exec {
        POST "/exec/{id}/start" => start(id: &str, start: Start) -> TcpTunnel;
    }
}

const RAW_STREAM: &str = "application/vnd.docker.raw-stream";

/// What the daemon saw: the headers of each request, and the end of each tunnel, once the client's half has closed
/// or gone.
#[derive(Debug, Default)]
struct Watch {
    headers: Mutex<Vec<HeaderMap>>,
    ended: Notify,
}

type Seen = Arc<Watch>;

/// The daemon: the id of the exec says what it answers. `echo` switches to `tcp` and echoes in capitals what it
/// reads until its end, then writes `BYE` and closes; the others answer what the client's check refuses.
fn daemon(seen: Seen) -> Router {
    Router::new().route(
        "/v1/exec/{id}/start",
        post(move |Path(id): Path<String>, request: Request| {
            let seen = Arc::clone(&seen);

            async move { start(seen, id, request).await }
        }),
    )
}

async fn start(seen: Seen, id: String, request: Request) -> Response {
    let (mut parts, body) = request.into_parts();

    seen.headers
        .lock()
        .expect("no test panics holding the lock")
        .push(parts.headers.clone());

    let body = axum::body::to_bytes(body, 1024).await.expect("a body");
    let start: Start = serde_json::from_slice(&body).expect("the body of a start");

    assert_eq!(start, Start { tty: true });

    let switched = match id.as_str() {
        "missing" => {
            return Response::builder()
                .status(StatusCode::NOT_FOUND)
                .header(header::CONTENT_TYPE, "application/json")
                .body(Body::from(
                    r#"{"message":"No such exec instance: missing"}"#,
                ))
                .expect("a response");
        }
        // Docker's answer to a request that asks for no upgrade: a 200, then the connection hijacked.
        "ok" => {
            return Response::builder()
                .status(StatusCode::OK)
                .body(Body::from("hijacked"))
                .expect("a response");
        }
        "empty" => return StatusCode::NO_CONTENT.into_response(),
        "h2c" => Some("h2c"),
        "list" => Some("tcp, h2c"),
        "none" => None,
        "capitals" => Some(" TCP "),
        _ => Some("tcp"),
    };

    if switched.is_some_and(|protocol| protocol.trim().eq_ignore_ascii_case("tcp")) {
        let upgrade = parts
            .extensions
            .remove::<hyper::upgrade::OnUpgrade>()
            .expect("a connection that may be upgraded");

        tokio::spawn(async move {
            let mut io = TokioIo::new(upgrade.await.expect("the connection upgraded"));
            let mut buffer = [0; 1024];

            // Until the client's half ends, closed or gone: a reset is an end too.
            while let Ok(read @ 1..) = io.read(&mut buffer).await {
                if io
                    .write_all(&buffer[..read].to_ascii_uppercase())
                    .await
                    .is_err()
                {
                    break;
                }
            }

            // A client that has gone reads none of it.
            let _ = io.write_all(b"BYE").await;
            let _ = io.shutdown().await;

            seen.ended.notify_one();
        });
    }

    let mut response = Response::builder()
        .status(StatusCode::SWITCHING_PROTOCOLS)
        .header(header::CONNECTION, "Upgrade")
        .header(header::CONTENT_TYPE, RAW_STREAM);

    if let Some(protocol) = switched {
        response = response.header(header::UPGRADE, protocol);
    }

    response.body(Body::empty()).expect("a response")
}

/// The daemon on port 0: its URL, and the headers it sees.
async fn on_tcp() -> (String, Seen) {
    let seen = Seen::default();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a free port");
    let address = listener.local_addr().expect("an address");

    tokio::spawn(axum::serve(listener, daemon(Arc::clone(&seen))).into_future());

    (format!("http://{address}"), seen)
}

/// Every helper of the client, with a deadline shorter than the exchange.
fn every_helper() -> client::Builder {
    client::builder()
        .with(client::trace())
        .with(client::user_agent("tunnel-test"))
        .with(client::request_id())
        .with(client::retry(2))
        .with(client::timeout(Duration::from_millis(100)))
        .with(client::redirects(3))
        .with(client::bearer("s3cr3t"))
        .with(client::on_request(
            |request: &mut reqwest::Request, _: &quoll::Route| {
                request
                    .headers_mut()
                    .insert("x-signed", HeaderValue::from_static("yes"));
            },
        ))
}

/// Opens the tunnel of `api` and talks on it, past the deadline of `client::timeout`: what the daemon saw, and that
/// a half-close lets the daemon's last bytes through.
async fn talks(api: &DaemonClient, seen: &Seen) {
    let mut tunnel = api
        .exec()
        .start("echo", Start { tty: true })
        .await
        .expect("a tunnel");

    assert_eq!(tunnel.content_type(), RAW_STREAM);

    let request = seen
        .headers
        .lock()
        .expect("no test panics holding the lock")
        .pop()
        .expect("a request");

    assert_eq!(request[header::AUTHORIZATION], "Bearer s3cr3t");
    assert_eq!(request[header::USER_AGENT], "tunnel-test");
    assert!(request.contains_key("x-request-id"), "{request:?}");
    assert_eq!(request["x-signed"], "yes");
    assert_eq!(request[header::CONNECTION], "Upgrade");
    assert_eq!(request[header::UPGRADE], "tcp");

    // A deadline that does not pass is seen only once its time has: `client::timeout` bounds the 101 alone, and
    // neither it nor reqwest's own deadline cuts the tunnel after it.
    tokio::time::sleep(Duration::from_millis(300)).await;

    tunnel.write_all(b"hello").await.expect("a write");

    let mut echoed = [0; 5];
    tunnel.read_exact(&mut echoed).await.expect("the echo");

    assert_eq!(&echoed, b"HELLO");

    tunnel.shutdown().await.expect("a half-close");

    let mut rest = Vec::new();
    tunnel.read_to_end(&mut rest).await.expect("the end");

    assert_eq!(rest, b"BYE");
}

#[tokio::test]
async fn a_tunnel_opens_through_every_helper_of_the_client() {
    let (url, seen) = on_tcp().await;
    let api = every_helper().build::<Daemon>(&url).expect("a client");

    talks(&api, &seen).await;
}

#[cfg(unix)]
#[tokio::test]
async fn a_tunnel_opens_over_a_unix_socket() {
    let directory = tempfile::tempdir().expect("a directory");
    let socket = directory.path().join("daemon.sock");
    let listener = tokio::net::UnixListener::bind(&socket).expect("a socket");
    let seen = Seen::default();

    tokio::spawn(axum::serve(listener, daemon(Arc::clone(&seen))).into_future());

    let api = every_helper()
        .http(move |http| http.unix_socket(socket))
        .build::<Daemon>("http://localhost")
        .expect("a client");

    talks(&api, &seen).await;
}

#[tokio::test]
async fn a_tunnel_opens_with_no_middleware() {
    let (url, _) = on_tcp().await;
    let api = Daemon::client(&url).expect("a client");

    let mut tunnel = api
        .exec()
        .start("echo", Start { tty: true })
        .await
        .expect("a tunnel");

    tunnel.write_all(b"x").await.expect("a write");

    let mut echoed = [0; 1];
    tunnel.read_exact(&mut echoed).await.expect("the echo");

    assert_eq!(&echoed, b"X");
}

#[tokio::test]
async fn the_response_is_the_whole_101() {
    let (url, _) = on_tcp().await;
    let api = Daemon::client(&url).expect("a client");

    let response = api
        .exec()
        .start("echo", Start { tty: true })
        .response()
        .await
        .expect("an answer");

    assert_eq!(response.status(), StatusCode::SWITCHING_PROTOCOLS);
    assert_eq!(response.headers()[header::UPGRADE], "tcp");

    let mut tunnel = response.into_value().expect("a tunnel");

    tunnel.write_all(b"y").await.expect("a write");

    let mut echoed = [0; 1];
    tunnel.read_exact(&mut echoed).await.expect("the echo");

    assert_eq!(&echoed, b"Y");
}

#[tokio::test]
async fn only_a_101_to_tcp_alone_gives_a_tunnel() {
    let (url, _) = on_tcp().await;
    let api = Daemon::client(&url).expect("a client");

    let mut tunnel = api
        .exec()
        .start("capitals", Start { tty: true })
        .await
        .expect("`tcp`, its case and the blanks around it ignored");

    tunnel.write_all(b"z").await.expect("a write");

    let mut echoed = [0; 1];
    tunnel.read_exact(&mut echoed).await.expect("the echo");

    assert_eq!(&echoed, b"Z");

    let refused = [
        (
            "ok",
            "the call `exec_start` asked for a TCP tunnel and was answered 200 OK, not 101 Switching Protocols",
        ),
        (
            "h2c",
            "the call `exec_start` asked for a TCP tunnel and was switched to `h2c`",
        ),
        (
            "list",
            "the call `exec_start` asked for a TCP tunnel and was switched to `tcp, h2c`",
        ),
        (
            "none",
            "the call `exec_start` asked for a TCP tunnel and was answered 101 with no `Upgrade` header",
        ),
    ];

    for (id, message) in refused {
        let error = api
            .exec()
            .start(id, Start { tty: true })
            .await
            .expect_err("no tunnel");

        assert_eq!(error.kind(), ErrorKind::Decode, "{id}: {error:?}");
        assert_eq!(error.status(), None, "{id}");
        assert_eq!(error.to_string(), message, "{id}");

        // `.response()` has no usable answer either: the answer is not the route's result.
        let error = api
            .exec()
            .start(id, Start { tty: true })
            .response()
            .await
            .expect_err("no usable answer");

        assert_eq!(error.kind(), ErrorKind::Decode, "{id}: {error:?}");
    }
}

#[tokio::test]
async fn a_204_is_no_tunnel_and_keeps_its_status_in_the_response() {
    let (url, _) = on_tcp().await;
    let api = Daemon::client(&url).expect("a client");

    let error = api
        .exec()
        .start("empty", Start { tty: true })
        .await
        .expect_err("no tunnel");

    assert_eq!(error.kind(), ErrorKind::Decode);
    assert_eq!(
        error.to_string(),
        "the call `exec_start` asked for a TCP tunnel and was answered 204 No Content, not 101 Switching Protocols"
    );

    // As on any route of a value: the answer of no content is the read error, with its status.
    let response = api
        .exec()
        .start("empty", Start { tty: true })
        .response()
        .await
        .expect("an answer");

    assert_eq!(response.status(), StatusCode::NO_CONTENT);
    assert_eq!(
        response.into_value().expect_err("no tunnel").kind(),
        ErrorKind::Decode
    );
}

#[tokio::test]
async fn an_error_answer_is_read_as_on_any_route() {
    let (url, _) = on_tcp().await;
    let api = Daemon::client(&url).expect("a client");

    let error = api
        .exec()
        .start("missing", Start { tty: true })
        .await
        .expect_err("no such exec");

    assert_eq!(error.kind(), ErrorKind::Status(StatusCode::NOT_FOUND));
    assert_eq!(
        error.detail().map(|refused| refused.message.as_str()),
        Some("No such exec instance: missing")
    );
    assert_eq!(
        error.to_string(),
        "another api answered 404: No such exec instance: missing"
    );

    let response = api
        .exec()
        .start("missing", Start { tty: true })
        .response()
        .await
        .expect("an answer");

    assert_eq!(response.status(), StatusCode::NOT_FOUND);
    assert!(response.into_value().is_err());
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

/// A middleware of the application's that gives a response of its own making, as a cache does: the 101 no longer
/// holds the connection hyper would give back.
fn remade() -> impl Middleware {
    middleware(|request, extensions, next| {
        Box::pin(async move {
            let answer = next.run(request, extensions).await?;
            let mut remade = http::Response::builder().status(answer.status());

            for (name, value) in answer.headers() {
                remade = remade.header(name, value);
            }

            let remade = remade.body(Vec::new()).expect("a response");

            Ok(reqwest::Response::from(remade))
        })
    })
}

#[tokio::test]
async fn a_101_whose_connection_cannot_be_taken_is_closed() {
    let (url, _) = on_tcp().await;
    let api = client::builder()
        .with(remade())
        .build::<Daemon>(&url)
        .expect("a client");

    let error = api
        .exec()
        .start("echo", Start { tty: true })
        .await
        .expect_err("no connection to take");

    assert_eq!(error.kind(), ErrorKind::Closed, "{error:?}");
    assert_eq!(error.status(), None);
    assert_eq!(
        error.to_string(),
        "the call `exec_start` could not take its TCP tunnel"
    );
}

#[tokio::test]
async fn dropping_the_tunnel_closes_its_connection() {
    let (url, seen) = on_tcp().await;
    let api = Daemon::client(&url).expect("a client");
    let mut tunnel = api
        .exec()
        .start("echo", Start { tty: true })
        .await
        .expect("a tunnel");

    tunnel.write_all(b"a").await.expect("a write");

    let mut echoed = [0; 1];
    tunnel.read_exact(&mut echoed).await.expect("the echo");

    drop(tunnel);

    tokio::time::timeout(Duration::from_secs(10), seen.ended.notified())
        .await
        .expect("the daemon's end of the tunnel ends");
}

/// A fake of the group, its tunnel half of a `tokio::io::duplex` whose other half a task plays the daemon on.
struct FakeExec;

impl Exec for FakeExec {
    fn start(&self, id: &str, _start: Start) -> impl Call<TcpTunnel, Refused> {
        async move {
            if id != "e1" {
                return Err(quoll::Error::answered(
                    StatusCode::NOT_FOUND,
                    Refused {
                        message: format!("No such exec instance: {id}"),
                    },
                ));
            }

            let (ours, mut theirs) = tokio::io::duplex(64);

            tokio::spawn(async move {
                let mut line = Vec::new();

                theirs.read_to_end(&mut line).await.expect("a read");
                theirs
                    .write_all(&line.to_ascii_uppercase())
                    .await
                    .expect("a write");
            });

            Ok(TcpTunnel::new(ours).with_content_type(RAW_STREAM))
        }
    }
}

/// Code written for the client, which a fake runs as well.
async fn shout(exec: &impl Exec, id: &str, text: &[u8]) -> quoll::Result<Vec<u8>, Refused> {
    let mut tunnel = exec.start(id, Start { tty: true }).await?;
    let mut shouted = Vec::new();

    tunnel.write_all(text).await.expect("a write");
    tunnel.shutdown().await.expect("a half-close");
    tunnel.read_to_end(&mut shouted).await.expect("the end");

    Ok(shouted)
}

#[tokio::test]
async fn a_fake_gives_a_tunnel_on_a_duplex() {
    assert_eq!(
        shout(&FakeExec, "e1", b"hello").await.expect("a tunnel"),
        b"HELLO"
    );

    let error = shout(&FakeExec, "e2", b"hello")
        .await
        .expect_err("no such exec");

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));

    let response = FakeExec
        .start("e1", Start { tty: true })
        .response()
        .await
        .expect("an answer");

    // A fake answers 200, not the 101 of the contract, as a fake of any route answers its own status.
    assert_eq!(response.status(), StatusCode::OK);
    assert!(
        response
            .into_value()
            .expect("a tunnel")
            .has_type(RAW_STREAM)
    );
}
