//! The command line against a fake daemon, served on port 0 and on a Unix socket: a router written without the
//! contract, which answers as Docker does, its quirks included.

use std::collections::HashMap;
use std::process::Output;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::Router;
use axum::body::Body;
use axum::extract::{Path, Query, State};
use axum::http::{StatusCode, header};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use futures::stream;
use serde::Deserialize;
use tokio::process::Command;

/// A bound, so that a command that does not end fails the test rather than hang it.
const BOUND: Duration = Duration::from_secs(60);

const WEB: &str = "c0ffee000000000000000000000000000000000000000000000000000000c0de";
const JOB: &str = "deadbeef0000000000000000000000000000000000000000000000000000beef";
const SHELL: &str = "7777777700000000000000000000000000000000000000000000000000007777";

/// What the fake daemon holds, and what it was asked.
#[derive(Debug)]
struct Daemon {
    /// Each container's id, its name, whether it runs, and whether it has a TTY.
    containers: Vec<(&'static str, &'static str, bool, bool)>,
    /// The query of each call, by its path.
    queries: Vec<(String, HashMap<String, String>)>,
}

type Shared = Arc<Mutex<Daemon>>;

fn daemon() -> Shared {
    Arc::new(Mutex::new(Daemon {
        containers: vec![
            (WEB, "web", true, false),
            (JOB, "job", false, false),
            (SHELL, "shell", true, true),
        ],
        queries: Vec::new(),
    }))
}

/// Docker's answer to a container it does not know: its own error shape, not an RFC 9457 problem.
fn no_such(id: &str) -> Response {
    let body = format!(r#"{{"message":"No such container: {id}"}}"#);

    (
        StatusCode::NOT_FOUND,
        [(header::CONTENT_TYPE, "application/json")],
        body,
    )
        .into_response()
}

/// The container `id` names, by its id, its short id or its name.
fn find(daemon: &Daemon, id: &str) -> Option<usize> {
    daemon.containers.iter().position(|(full, name, ..)| {
        *full == id || (id.len() >= 12 && full.starts_with(id)) || *name == id
    })
}

/// The bytes of a frame of `payload`, written to `stream`.
fn frame(stream: u8, payload: &[u8]) -> Vec<u8> {
    let size = u32::try_from(payload.len()).expect("a small payload");
    let mut frame = vec![stream, 0, 0, 0];

    frame.extend(size.to_be_bytes());
    frame.extend(payload);
    frame
}

/// `bytes` sent in chunks of `size`, so that frames and lines are cut anywhere.
fn chunked(bytes: Vec<u8>, size: usize) -> Body {
    let chunks: Vec<Result<Vec<u8>, std::convert::Infallible>> =
        bytes.chunks(size).map(|chunk| Ok(chunk.to_vec())).collect();

    Body::from_stream(stream::iter(chunks))
}

fn recorded(daemon: &Shared, path: String, query: HashMap<String, String>) {
    daemon
        .lock()
        .expect("no test panics holding the lock")
        .queries
        .push((path, query));
}

#[derive(Deserialize)]
struct Id {
    id: String,
}

fn fake(daemon: Shared) -> Router {
    Router::new()
        .route("/v1.47/containers/json", get(list))
        .route("/v1.47/containers/{id}/json", get(inspect))
        .route("/v1.47/containers/{id}/start", post(start))
        .route("/v1.47/containers/{id}/stop", post(stop))
        .route("/v1.47/containers/{id}/logs", get(logs))
        .route("/v1.47/events", get(events))
        .with_state(daemon)
}

async fn list(
    State(daemon): State<Shared>,
    Query(query): Query<HashMap<String, String>>,
) -> Response {
    let all = query.get("all").is_some_and(|all| all == "true");

    recorded(&daemon, "list".into(), query);

    let daemon = daemon.lock().expect("no test panics holding the lock");
    let containers: Vec<_> = daemon
        .containers
        .iter()
        .filter(|(_, _, running, _)| all || *running)
        .map(|(id, name, running, _)| {
            let (state, status) = if *running {
                ("running", "Up 2 minutes")
            } else {
                ("exited", "Exited (0) 3 hours ago")
            };

            serde_json::json!({
                "Id": id,
                "Names": [format!("/{name}")],
                "Image": "alpine:3",
                "State": state,
                "Status": status,
                "Labels": {},
            })
        })
        .collect();

    axum::Json(containers).into_response()
}

async fn inspect(State(daemon): State<Shared>, Path(Id { id }): Path<Id>) -> Response {
    let daemon = daemon.lock().expect("no test panics holding the lock");
    let Some(index) = find(&daemon, &id) else {
        return no_such(&id);
    };
    let (full, name, running, tty) = daemon.containers[index];

    axum::Json(serde_json::json!({
        "Id": full,
        "Name": format!("/{name}"),
        "Created": "2026-11-20T10:00:00.000000000Z",
        "Config": { "Image": "alpine:3", "Tty": tty, "Env": [] },
        "State": {
            "Status": if running { "running" } else { "exited" },
            "Running": running,
            "ExitCode": 0,
            "StartedAt": "2026-11-20T10:00:01.000000000Z",
            "FinishedAt": "0001-01-01T00:00:00Z",
        },
    }))
    .into_response()
}

/// Sets the container's state to `running`: 204, or 304 when it was so already.
fn set_running(daemon: &Shared, id: &str, running: bool) -> Response {
    let mut daemon = daemon.lock().expect("no test panics holding the lock");
    let Some(index) = find(&daemon, id) else {
        return no_such(id);
    };

    if daemon.containers[index].2 == running {
        return StatusCode::NOT_MODIFIED.into_response();
    }

    daemon.containers[index].2 = running;
    StatusCode::NO_CONTENT.into_response()
}

async fn start(State(daemon): State<Shared>, Path(Id { id }): Path<Id>) -> Response {
    set_running(&daemon, &id, true)
}

async fn stop(
    State(daemon): State<Shared>,
    Path(Id { id }): Path<Id>,
    Query(query): Query<HashMap<String, String>>,
) -> Response {
    recorded(&daemon, "stop".into(), query);
    set_running(&daemon, &id, false)
}

async fn logs(
    State(daemon): State<Shared>,
    Path(Id { id }): Path<Id>,
    Query(query): Query<HashMap<String, String>>,
) -> Response {
    recorded(&daemon, "logs".into(), query);

    let tty = {
        let daemon = daemon.lock().expect("no test panics holding the lock");
        let Some(index) = find(&daemon, &id) else {
            return no_such(&id);
        };

        daemon.containers[index].3
    };

    if tty {
        return (
            [(header::CONTENT_TYPE, "application/vnd.docker.raw-stream")],
            chunked(b"$ ls\r\nbin  etc\r\n".to_vec(), 5),
        )
            .into_response();
    }

    let frames = [
        frame(1, b"hello\n"),
        frame(2, b"oops\n"),
        frame(1, b"bye\n"),
    ]
    .concat();

    (
        [(
            header::CONTENT_TYPE,
            "application/vnd.docker.multiplexed-stream",
        )],
        chunked(frames, 3),
    )
        .into_response()
}

/// Two events, one between them that is none the command line reads, then the end: Docker before 29 sends them as
/// `application/json`, one a line.
async fn events(
    State(daemon): State<Shared>,
    Query(query): Query<HashMap<String, String>>,
) -> Response {
    recorded(&daemon, "events".into(), query);

    let lines = concat!(
        r#"{"Type":"container","Action":"start","Actor":{"ID":"c0ffee000000000000000000","Attributes":{"image":"alpine:3","name":"web"}},"scope":"local","time":1763632800,"timeNano":1763632800000000000}"#,
        "\n",
        r#"{"Type":"container","scope":"local"}"#,
        "\n",
        r#"{"Type":"network","Action":"connect","Actor":{"ID":"5e7000000000000000000000","Attributes":{"name":"bridge"}},"scope":"local","time":1763632801,"timeNano":1763632801000000000}"#,
        "\n",
    );

    (
        [(header::CONTENT_TYPE, "application/json")],
        chunked(lines.as_bytes().to_vec(), 40),
    )
        .into_response()
}

/// The fake daemon on port 0: its `DOCKER_HOST`, and what it holds.
async fn on_tcp() -> (String, Shared) {
    let daemon = daemon();
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a free port");
    let address = listener.local_addr().expect("an address");

    tokio::spawn(axum::serve(listener, fake(Arc::clone(&daemon))).into_future());

    (format!("tcp://{address}"), daemon)
}

/// The command line run with `args` against the daemon at `docker_host`, to its end.
async fn docker_cli(docker_host: &str, args: &[&str]) -> Output {
    let run = Command::new(env!("CARGO_BIN_EXE_docker-cli"))
        .args(args)
        .env("DOCKER_HOST", docker_host)
        .env_remove("DOCKER_TLS_VERIFY")
        .kill_on_drop(true)
        .output();

    tokio::time::timeout(BOUND, run)
        .await
        .expect("the command line ends")
        .expect("the command line runs")
}

/// The standard output of a command that succeeded.
fn succeeded(output: &Output) -> String {
    assert!(
        output.status.success(),
        "{:?}: {}",
        output.status,
        String::from_utf8_lossy(&output.stderr)
    );

    String::from_utf8(output.stdout.clone()).expect("UTF-8")
}

/// The queries the daemon got for `path`.
fn queries(daemon: &Shared, path: &str) -> Vec<HashMap<String, String>> {
    daemon
        .lock()
        .expect("no test panics holding the lock")
        .queries
        .iter()
        .filter(|(called, _)| called == path)
        .map(|(_, query)| query.clone())
        .collect()
}

#[tokio::test]
async fn ps_lists_the_running_containers_and_all_of_them_with_a() {
    let (host, daemon) = on_tcp().await;

    let running = succeeded(&docker_cli(&host, &["ps"]).await);
    let all = succeeded(&docker_cli(&host, &["ps", "-a"]).await);

    assert_eq!(
        running.lines().collect::<Vec<_>>(),
        [
            "CONTAINER ID  IMAGE                     STATUS                        NAMES",
            "c0ffee000000  alpine:3                  Up 2 minutes                  web",
            "777777770000  alpine:3                  Up 2 minutes                  shell",
        ]
    );
    assert!(
        all.contains("deadbeef0000  alpine:3                  Exited (0) 3 hours ago        job"),
        "{all}"
    );

    let alls: Vec<_> = queries(&daemon, "list")
        .iter()
        .map(|query| query["all"].clone())
        .collect();

    assert_eq!(alls, ["false", "true"]);
}

#[tokio::test]
async fn inspect_shows_a_container_and_says_dockers_message_for_another() {
    let (host, _) = on_tcp().await;

    let shown = succeeded(&docker_cli(&host, &["inspect", "web"]).await);
    let inspected: serde_json::Value = serde_json::from_str(&shown).expect("JSON");

    assert_eq!(inspected["Id"], WEB);
    assert_eq!(inspected["State"]["Running"], true);

    let missing = docker_cli(&host, &["inspect", "nope"]).await;
    let stderr = String::from_utf8_lossy(&missing.stderr);

    assert!(!missing.status.success());
    assert!(
        stderr.contains("Error: another api answered 404: No such container: nope"),
        "{stderr}"
    );
}

#[tokio::test]
async fn start_and_stop_say_when_there_was_nothing_to_do() {
    let (host, daemon) = on_tcp().await;

    assert_eq!(
        succeeded(&docker_cli(&host, &["start", "job"]).await),
        "job\n"
    );
    assert_eq!(
        succeeded(&docker_cli(&host, &["start", "job"]).await),
        "job is running already\n"
    );
    assert_eq!(
        succeeded(&docker_cli(&host, &["stop", "job", "-t", "3"]).await),
        "job\n"
    );
    assert_eq!(
        succeeded(&docker_cli(&host, &["stop", "job"]).await),
        "job is stopped already\n"
    );

    let timeouts: Vec<_> = queries(&daemon, "stop")
        .iter()
        .map(|query| query.get("t").cloned())
        .collect();

    assert_eq!(timeouts, [Some("3".to_owned()), None]);

    let missing = docker_cli(&host, &["start", "nope"]).await;

    assert!(!missing.status.success());
    assert!(String::from_utf8_lossy(&missing.stderr).contains("No such container: nope"));
}

#[tokio::test]
async fn logs_write_each_stream_to_its_own() {
    let (host, daemon) = on_tcp().await;

    let output = docker_cli(&host, &["logs", "web"]).await;

    assert_eq!(succeeded(&output), "hello\nbye\n");
    assert_eq!(String::from_utf8_lossy(&output.stderr), "oops\n");

    let followed = docker_cli(&host, &["logs", "-f", "web"]).await;

    assert_eq!(succeeded(&followed), "hello\nbye\n");

    let follows: Vec<_> = queries(&daemon, "logs")
        .iter()
        .map(|query| {
            (
                query["follow"].clone(),
                query["stdout"].clone(),
                query["stderr"].clone(),
            )
        })
        .collect();

    assert_eq!(
        follows,
        [
            ("false".to_owned(), "true".to_owned(), "true".to_owned()),
            ("true".to_owned(), "true".to_owned(), "true".to_owned()),
        ]
    );
}

#[tokio::test]
async fn the_logs_of_a_tty_are_written_as_they_are() {
    let (host, _) = on_tcp().await;

    let output = docker_cli(&host, &["logs", "shell"]).await;

    assert_eq!(succeeded(&output), "$ ls\r\nbin  etc\r\n");
    assert!(output.stderr.is_empty());
}

#[tokio::test]
async fn events_are_written_one_a_line_and_one_not_read_is_skipped() {
    let (host, daemon) = on_tcp().await;

    let output = docker_cli(&host, &["events"]).await;
    let stderr = String::from_utf8_lossy(&output.stderr);

    assert_eq!(
        succeeded(&output).lines().collect::<Vec<_>>(),
        [
            "1763632800 container start c0ffee000000 web",
            "1763632801 network connect 5e7000000000 bridge",
        ]
    );
    assert_eq!(stderr.lines().count(), 1, "{stderr}");
    assert!(stderr.starts_with("warning: an event is skipped: the call `system_events` got an item that does not read as its type: missing field"), "{stderr}");

    succeeded(&docker_cli(&host, &["events", "--since", "1763632700"]).await);

    let since: Vec<_> = queries(&daemon, "events")
        .iter()
        .map(|query| query.get("since").cloned())
        .collect();

    assert_eq!(since, [None, Some("1763632700".to_owned())]);
}

#[cfg(unix)]
#[tokio::test]
async fn the_daemon_is_reached_on_its_unix_socket() {
    let directory = std::env::temp_dir().join(format!("docker-cli-test-{}", std::process::id()));
    let socket = directory.join("docker.sock");

    std::fs::create_dir_all(&directory).expect("a directory");
    let _ = std::fs::remove_file(&socket);

    let listener = tokio::net::UnixListener::bind(&socket).expect("a socket");

    tokio::spawn(axum::serve(listener, fake(daemon())).into_future());

    let running = succeeded(&docker_cli(&format!("unix://{}", socket.display()), &["ps"]).await);

    assert!(running.contains("c0ffee000000"), "{running}");
    let _ = std::fs::remove_dir_all(&directory);
}

#[tokio::test]
async fn a_host_it_does_not_reach_says_what_to_write() {
    let output = docker_cli("ssh://me@docker", &["ps"]).await;
    let stderr = String::from_utf8_lossy(&output.stderr);

    assert!(!output.status.success());
    assert!(
        stderr.contains("write `unix:///path` or `tcp://host:port`"),
        "{stderr}"
    );
}
