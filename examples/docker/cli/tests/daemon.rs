#![cfg(unix)]

//! The command line against a real daemon, where one answers: the one `DOCKER_HOST` names, or Docker's own socket.
//!
//! With no daemon, the test says so and passes, unless `DOCKER_CLI_TEST_DAEMON` is `required`, as CI sets it on the
//! runners that have Docker: there, a daemon that does not answer fails it.

use std::process::Stdio;
use std::time::Duration;

use docker_api::{Containers, Docker, DockerClient, Images, NewContainer, System};
use futures::StreamExt;
use quoll::client;
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::{Child, Command};

/// A bound, so that a command or a stream that does not end fails the test rather than hang it.
const BOUND: Duration = Duration::from_secs(120);

/// A small image, which has a shell.
const IMAGE: &str = "alpine";
const TAG: &str = "3";

/// A client of the daemon `DOCKER_HOST` names, as the command line reads it, for what the test sets up.
fn docker() -> DockerClient {
    let docker_host = std::env::var("DOCKER_HOST").unwrap_or_default();

    if let Some(address) = docker_host.strip_prefix("tcp://") {
        return Docker::client(&format!("http://{address}")).expect("an http url");
    }

    let socket = docker_host
        .strip_prefix("unix://")
        .unwrap_or("/var/run/docker.sock")
        .to_owned();

    client::builder()
        .http(move |http| http.unix_socket(socket))
        .build::<Docker>("http://localhost")
        .expect("a client")
}

/// The command line run with `args`, to its end: its standard output.
async fn docker_cli(args: &[&str]) -> String {
    let run = Command::new(env!("CARGO_BIN_EXE_docker-cli"))
        .args(args)
        .kill_on_drop(true)
        .output();
    let output = tokio::time::timeout(BOUND, run)
        .await
        .expect("the command line ends")
        .expect("the command line runs");

    assert!(
        output.status.success(),
        "{args:?}, {:?}: {}",
        output.status,
        String::from_utf8_lossy(&output.stderr)
    );
    String::from_utf8(output.stdout).expect("UTF-8")
}

/// The command line run with `args`, left running, its standard output and its error piped.
fn spawned(args: &[&str]) -> Child {
    Command::new(env!("CARGO_BIN_EXE_docker-cli"))
        .args(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .expect("the command line runs")
}

/// Reads the lines of `lines` until one holds each of `wanted`.
async fn line_with(lines: &mut (impl tokio::io::AsyncBufRead + Unpin), wanted: &[&str]) -> String {
    let read = async {
        let mut line = String::new();

        loop {
            line.clear();

            let read = lines.read_line(&mut line).await.expect("a line");

            assert!(read > 0, "the output ended before a line held {wanted:?}");

            if wanted.iter().all(|wanted| line.contains(wanted)) {
                return line.clone();
            }
        }
    };

    tokio::time::timeout(BOUND, read)
        .await
        .unwrap_or_else(|_| panic!("no line held {wanted:?}"))
}

#[tokio::test]
async fn each_command_works_on_a_real_daemon() {
    let docker = docker();

    if let Err(error) = docker.system().version().await {
        let required =
            std::env::var("DOCKER_CLI_TEST_DAEMON").is_ok_and(|value| value == "required");

        assert!(
            !required,
            "DOCKER_CLI_TEST_DAEMON is required, and no daemon answers: {error:?}"
        );
        eprintln!(
            "skipped: no Docker daemon answers, at DOCKER_HOST or /var/run/docker.sock ({error}); set DOCKER_CLI_TEST_DAEMON=required to fail instead"
        );
        return;
    }

    let mut progress = docker
        .images()
        .pull(IMAGE.into(), TAG.into())
        .await
        .expect("the pull starts");

    while let Some(line) = progress.next().await {
        let line = line.expect("a line of progress");

        assert!(line.error.is_none(), "the pull failed: {line:?}");
    }

    let name = format!("docker-cli-test-{}", std::process::id());
    let container = NewContainer {
        image: format!("{IMAGE}:{TAG}"),
        cmd: ["sh", "-c", "echo out; echo err >&2; exec sleep 300"]
            .map(String::from)
            .to_vec(),
        tty: false,
    };

    docker
        .containers()
        .create(Some(name.clone()), container)
        .await
        .expect("the container is created");

    assert!(docker_cli(&["ps", "-a"]).await.contains(&name));

    let mut events = spawned(&["events"]);
    let mut event_lines = BufReader::new(events.stdout.take().expect("piped"));

    assert_eq!(docker_cli(&["start", &name]).await, format!("{name}\n"));
    assert_eq!(
        docker_cli(&["start", &name]).await,
        format!("{name} is running already\n")
    );
    line_with(&mut event_lines, &["container start", &name]).await;
    events.kill().await.expect("the events stop");

    let mut logs = spawned(&["logs", "-f", &name]);
    let mut stdout = BufReader::new(logs.stdout.take().expect("piped"));
    let mut stderr = BufReader::new(logs.stderr.take().expect("piped"));

    assert_eq!(line_with(&mut stdout, &["out"]).await, "out\n");
    assert_eq!(line_with(&mut stderr, &["err"]).await, "err\n");
    logs.kill().await.expect("the logs stop");

    assert!(
        docker_cli(&["inspect", &name])
            .await
            .contains("\"Running\": true")
    );
    assert_eq!(
        docker_cli(&["stop", &name, "-t", "1"]).await,
        format!("{name}\n")
    );
    assert_eq!(
        docker_cli(&["stop", &name]).await,
        format!("{name} is stopped already\n")
    );

    docker
        .containers()
        .remove(name, true)
        .await
        .expect("the container is removed");
}
