//! `client::follow` against a real server on port 0, through the real client, under paused time: a server of raw TCP
//! that answers then closes, cuts its body, closes with no answer, refuses, holds, answers an error or a page, or ends
//! its events with a problem.
//!
//! Paused time skips to the next timer as soon as the runtime waits on nothing else, a socket included. So a timer
//! that is pending while the test waits on the server may fire first: a deadline of the client, which would fail an
//! attempt that was answered, or the `steady` of `follow`, which would count a connection as held. These tests
//! therefore give the client no deadline, and check the waits only where no connection is open: a refusal, a
//! connection closed with no answer, an answer that is no stream, and a connection held an hour, long past `steady`.
//! The rules of the waits are checked on a fake, in `follow.rs`.

#![cfg(feature = "client")]

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures_util::{Stream, StreamExt};
use quoll::client::{Followed, Reconnect};
use quoll::{Events, JsonLines};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpSocket, TcpStream};
use tokio::time::Instant;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Change {
    kind: String,
}

quoll::api! {
    api Api = "";

    group Feed {
        GET "/lines"  => lines() -> JsonLines<Change>;
        GET "/events" => events() -> Events<Change>;
        GET "/since"  => since(header("X-Since") since: &str) -> JsonLines<Change>;
    }
}

/// What the server does with a connection.
#[derive(Clone)]
enum Answer {
    /// A 200 with these lines, then a clean end.
    Lines(Vec<&'static str>),
    /// A 200 with these lines, then the connection closed inside the body.
    Cut(Vec<&'static str>),
    /// A 200 with these lines, then this long before a clean end.
    HoldsFor(Vec<&'static str>, Duration),
    /// The connection closed before any answer.
    Closes,
    /// An answer of this status line and problem.
    Status(&'static str, &'static str),
    /// A 200 of server-sent events, as they are written.
    Events(&'static str),
    /// A 200 of this content type and body.
    Typed(&'static str, &'static str),
}

/// A server on a free port that answers each connection as `script` says, and counts them.
async fn serve(script: impl IntoIterator<Item = Answer>) -> (String, Arc<Mutex<usize>>) {
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("a free port");
    let address = listener.local_addr().expect("an address");
    let script = Arc::new(Mutex::new(script.into_iter().collect::<VecDeque<_>>()));
    let count = Arc::new(Mutex::new(0));
    let counted = count.clone();

    tokio::spawn(async move {
        loop {
            let (socket, _) = listener.accept().await.expect("a connection");
            let answer = script
                .lock()
                .expect("no test panics with the lock")
                .pop_front();

            *counted.lock().expect("no test panics with the lock") += 1;
            tokio::spawn(answer_with(socket, answer));
        }
    });

    (format!("http://{address}"), count)
}

async fn answer_with(mut socket: TcpStream, answer: Option<Answer>) {
    let mut head = Vec::new();
    let mut buffer = [0; 1024];

    while !head.windows(4).any(|window| window == b"\r\n\r\n") {
        let read = socket.read(&mut buffer).await.expect("a request");

        if read == 0 {
            return;
        }

        head.extend_from_slice(&buffer[..read]);
    }

    let Some(answer) = answer else {
        // Past the script, the server holds the connection with no answer.
        return std::future::pending().await;
    };
    let chunked = "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    let chunk = |text: &str| format!("{:x}\r\n{text}\r\n", text.len());
    let jsonl = |lines: &[&str]| {
        let body: String = lines
            .iter()
            .map(|line| chunk(&format!("{line}\n")))
            .collect();

        format!("HTTP/1.1 200 OK\r\nContent-Type: application/jsonl\r\n{chunked}{body}")
    };

    let written = match answer {
        Answer::Lines(lines) => format!("{}0\r\n\r\n", jsonl(&lines)),
        Answer::Cut(lines) => jsonl(&lines),
        Answer::HoldsFor(lines, duration) => {
            socket
                .write_all(jsonl(&lines).as_bytes())
                .await
                .expect("written");
            tokio::time::sleep(duration).await;

            "0\r\n\r\n".to_owned()
        }
        Answer::Closes => String::new(),
        Answer::Status(status, body) => format!(
            "HTTP/1.1 {status}\r\nContent-Type: application/problem+json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        ),
        Answer::Events(events) => format!(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n{chunked}{}0\r\n\r\n",
            chunk(events)
        ),
        Answer::Typed(content_type, body) => format!(
            "HTTP/1.1 200 OK\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        ),
    };

    // A client that gave up has closed its end: what it does not read does not matter.
    let _ = socket.write_all(written.as_bytes()).await;
}

const NOTE: &str = r#"{"kind":"note"}"#;

/// What a followed state is, by name, with the kind of its error.
fn name(followed: &Followed<Change>) -> String {
    let name = match followed {
        Followed::Connected => "connected".to_owned(),
        Followed::Item(change) => format!("item {}", change.kind),
        Followed::Skipped(error) => format!("skipped {}", error.kind()),
        Followed::Reconnecting { error: None, wait } => {
            format!("ended, again in {}s", wait.as_secs())
        }
        Followed::Reconnecting {
            error: Some(error),
            wait,
        } => format!("failed {}, again in {}s", error.kind(), wait.as_secs()),
        Followed::Stopped(error) => format!("stopped {}: {error}", error.kind()),
    };

    // Shown as it comes, so that a test that does not end says where it is.
    eprintln!("{:?} {name}", Instant::now());
    name
}

/// The lines at `url`, followed with no jitter, through a client with no deadline.
fn lines(url: &str) -> impl Stream<Item = Followed<Change>> + Send + 'static {
    let feed = Api::client(url).expect("a client").feed();

    Reconnect::new()
        .without_jitter()
        .follow(async move || feed.lines().await)
}

/// The first `count` states that following the lines at `url` gives, by name.
async fn first(url: &str, count: usize) -> Vec<String> {
    lines(url)
        .take(count)
        .map(|followed| name(&followed))
        .collect()
        .await
}

/// The same, without the waits, which a connection open under paused time does not make certain.
async fn kinds(url: &str, count: usize) -> Vec<String> {
    let names = first(url, count).await;

    names
        .into_iter()
        .map(|name| name.split(", again").next().unwrap_or_default().to_owned())
        .collect()
}

#[tokio::test(start_paused = true)]
async fn follows_again_a_server_that_answers_then_closes() {
    let (url, connections) = serve([
        Answer::Lines(vec![NOTE]),
        Answer::Lines(vec![NOTE]),
        Answer::Lines(vec![]),
    ])
    .await;

    let followed = kinds(&url, 7).await;

    assert_eq!(
        followed,
        [
            "connected",
            "item note",
            "ended",
            "connected",
            "item note",
            "ended",
            "connected"
        ]
    );
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        3
    );
}

#[tokio::test(start_paused = true)]
async fn follows_again_after_a_body_cut() {
    let (url, _) = serve([Answer::Cut(vec![NOTE]), Answer::Lines(vec![])]).await;

    let followed = kinds(&url, 4).await;

    assert_eq!(
        followed,
        ["connected", "item note", "failed body", "connected"]
    );
}

#[tokio::test(start_paused = true)]
async fn skips_a_line_that_does_not_read_and_goes_on() {
    let (url, connections) = serve([Answer::Lines(vec!["not json", NOTE])]).await;

    let followed = kinds(&url, 4).await;

    assert_eq!(
        followed,
        ["connected", "skipped decode", "item note", "ended"]
    );
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        1
    );
}

#[tokio::test(start_paused = true)]
async fn follows_again_a_server_that_closes_with_no_answer() {
    let (url, connections) = serve([Answer::Closes, Answer::Closes, Answer::Lines(vec![])]).await;
    let started = Instant::now();

    let followed = first(&url, 3).await;

    assert_eq!(
        followed,
        [
            "failed closed, again in 1s",
            "failed closed, again in 2s",
            "connected"
        ]
    );
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        3
    );
    assert_eq!(started.elapsed(), Duration::from_secs(3));
}

#[tokio::test(start_paused = true)]
async fn follows_again_a_port_that_refuses() {
    // A socket bound but not listening holds its port, which refuses each connection. A port freed by dropping its
    // listener could be bound again by the server of another test.
    let socket = TcpSocket::new_v4().expect("a socket");

    socket
        .bind("127.0.0.1:0".parse().expect("an address"))
        .expect("a free port");

    let url = format!("http://{}", socket.local_addr().expect("an address"));
    let started = Instant::now();

    let followed = first(&url, 3).await;

    assert_eq!(
        followed,
        [
            "failed connect, again in 1s",
            "failed connect, again in 2s",
            "failed connect, again in 4s"
        ]
    );
    assert_eq!(started.elapsed(), Duration::from_secs(3));
}

#[tokio::test(start_paused = true)]
async fn holds_a_connection_that_stays_open() {
    let (url, connections) = serve([
        Answer::HoldsFor(vec![NOTE], Duration::from_secs(3600)),
        Answer::Lines(vec![]),
    ])
    .await;
    let started = Instant::now();

    let followed = first(&url, 4).await;

    // An hour later, the connection held: the first wait again.
    assert_eq!(
        followed,
        ["connected", "item note", "ended, again in 1s", "connected"]
    );
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        2
    );
    assert_eq!(started.elapsed(), Duration::from_secs(3600 + 1));
}

#[tokio::test(start_paused = true)]
async fn stops_on_a_404_after_a_503() {
    let (url, connections) = serve([
        Answer::Status(
            "503 Service Unavailable",
            r#"{"status":503,"detail":"starting"}"#,
        ),
        Answer::Status("404 Not Found", r#"{"status":404,"detail":"no such feed"}"#),
    ])
    .await;

    // One more than it gives: the stream ends after the 404.
    let followed: Vec<_> = lines(&url)
        .take(3)
        .map(|followed| name(&followed))
        .collect()
        .await;

    assert_eq!(
        followed,
        [
            "failed 503, again in 1s",
            "stopped 404: another api answered 404: no such feed"
        ]
    );
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        2
    );
}

#[tokio::test(start_paused = true)]
async fn stops_on_a_304() {
    let (url, connections) = serve([Answer::Status("304 Not Modified", "")]).await;

    let followed: Vec<_> = lines(&url).map(|followed| name(&followed)).collect().await;

    assert_eq!(followed.len(), 1);
    assert!(followed[0].starts_with("stopped 304"), "{followed:?}");
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        1
    );
}

#[tokio::test(start_paused = true)]
async fn follows_again_an_answer_of_another_content_type() {
    let (url, _) = serve([
        Answer::Typed("text/html", "<h1>Maintenance</h1>"),
        Answer::Lines(vec![]),
    ])
    .await;

    let followed = first(&url, 2).await;

    // The answer that is not the route's: no stream was open, so the wait is certain.
    assert_eq!(followed, ["failed decode, again in 1s", "connected"]);
}

#[tokio::test(start_paused = true)]
async fn follows_again_events_that_end_with_a_problem_whatever_its_status() {
    let (url, connections) = serve([
        Answer::Events(concat!(
            "data: {\"kind\":\"note\"}\n\n",
            "event: problem\ndata: {\"status\":503,\"detail\":\"the store went away\"}\n\n"
        )),
        Answer::Events(
            "event: problem\ndata: {\"status\":401,\"detail\":\"the token expired\"}\n\n",
        ),
        Answer::Events(""),
    ])
    .await;
    let feed = Api::client(&url).expect("a client").feed();

    let followed: Vec<_> = Reconnect::new()
        .without_jitter()
        .follow(async move || feed.events().await)
        .take(6)
        .map(|followed| name(&followed))
        .map(|name| name.split(", again").next().unwrap_or_default().to_owned())
        .collect()
        .await;

    assert_eq!(
        followed,
        [
            "connected",
            "item note",
            "failed 503",
            "connected",
            "failed 401",
            "connected"
        ]
    );
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        3
    );
}

#[tokio::test(start_paused = true)]
async fn stops_on_a_call_the_client_cannot_send() {
    let (url, connections) = serve([]).await;
    let feed = Api::client(&url).expect("a client").feed();

    let followed: Vec<_> = Reconnect::new()
        .without_jitter()
        .follow(async move || feed.since("a\nline break").await)
        .map(|followed| name(&followed))
        .collect()
        .await;

    assert_eq!(followed.len(), 1);
    assert!(followed[0].starts_with("stopped invalid"), "{followed:?}");
    assert_eq!(
        *connections.lock().expect("no test panics with the lock"),
        0
    );
}
