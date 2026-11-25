use std::process::ExitCode;

use anyhow::bail;
use docker_api::{Containers, DockerClient, Exec, ExecStart, NewExec, System};
use futures::StreamExt;
use quoll::ErrorKind;
use reqwest::StatusCode;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

use crate::args::Command;
use crate::frames::{Demux, Output, Piece};
use crate::utils::{error_chain, short_id};

/// The content type of the output of a TTY, the logs of a container or a command run in it: its bytes as they are, in
/// no frame.
const RAW_STREAM: &str = "application/vnd.docker.raw-stream";

/// Runs `command`: the code the command line exits with, the one of the command it ran for `exec`.
pub async fn run(docker: &DockerClient, command: Command) -> anyhow::Result<ExitCode> {
    match command {
        Command::Ps { all } => ps(docker, all).await?,
        Command::Inspect { container } => inspect(docker, container).await?,
        Command::Start { container } => start(docker, container).await?,
        Command::Stop { container, timeout } => stop(docker, container, timeout).await?,
        Command::Logs { container, follow } => logs(docker, container, follow).await?,
        Command::Exec {
            tty,
            container,
            command,
        } => return exec(docker, container, tty, command).await,
        Command::Events { since } => events(docker, since).await?,
    }

    Ok(ExitCode::SUCCESS)
}

async fn ps(docker: &DockerClient, all: bool) -> anyhow::Result<()> {
    let containers = docker.containers().list(all).await?;

    println!(
        "{:<12}  {:<24}  {:<28}  NAMES",
        "CONTAINER ID", "IMAGE", "STATUS"
    );

    for container in containers {
        let names: Vec<&str> = container
            .names
            .iter()
            .map(|name| name.trim_start_matches('/'))
            .collect();

        println!(
            "{:<12}  {:<24}  {:<28}  {}",
            short_id(&container.id),
            container.image,
            container.status,
            names.join(",")
        );
    }

    Ok(())
}

async fn inspect(docker: &DockerClient, container: String) -> anyhow::Result<()> {
    let inspected = docker.containers().inspect(&container).await?;

    println!("{}", serde_json::to_string_pretty(&inspected)?);
    Ok(())
}

async fn start(docker: &DockerClient, container: String) -> anyhow::Result<()> {
    match docker.containers().start(&container).await {
        Ok(()) => println!("{container}"),
        // Docker's answer for a container that runs already: no error to the caller, nothing done.
        Err(error) if error.status() == Some(StatusCode::NOT_MODIFIED) => {
            println!("{container} is running already");
        }
        Err(error) => return Err(error.into()),
    }

    Ok(())
}

async fn stop(
    docker: &DockerClient,
    container: String,
    timeout: Option<i32>,
) -> anyhow::Result<()> {
    match docker.containers().stop(&container, timeout).await {
        Ok(()) => println!("{container}"),
        Err(error) if error.status() == Some(StatusCode::NOT_MODIFIED) => {
            println!("{container} is stopped already");
        }
        Err(error) => return Err(error.into()),
    }

    Ok(())
}

async fn logs(docker: &DockerClient, container: String, follow: bool) -> anyhow::Result<()> {
    let mut logs = docker
        .containers()
        .logs(&container, follow, true, true)
        .await?;
    let framed = !logs.has_type(RAW_STREAM);
    let mut demux = Demux::default();
    let mut stdout = tokio::io::stdout();
    let mut stderr = tokio::io::stderr();

    while let Some(chunk) = logs.next().await {
        let chunk = chunk?;

        if !framed {
            stdout.write_all(&chunk).await?;
            stdout.flush().await?;
            continue;
        }

        write(demux.push(&chunk), &mut stdout, &mut stderr).await?;
    }

    Ok(demux.finish()?)
}

/// Writes the output of `pieces` each to its own stream, and shows it at once, as a followed container writes for
/// long: an error of the daemon comes after the output before it.
async fn write(
    pieces: Vec<Piece<'_>>,
    stdout: &mut tokio::io::Stdout,
    stderr: &mut tokio::io::Stderr,
) -> anyhow::Result<()> {
    let mut failure = None;

    for piece in pieces {
        match piece {
            Piece::Output(Output::Stdout, bytes) => stdout.write_all(bytes).await?,
            Piece::Output(Output::Stderr, bytes) => stderr.write_all(bytes).await?,
            Piece::Failed(error) => failure = Some(error),
        }
    }

    stdout.flush().await?;
    stderr.flush().await?;

    match failure {
        Some(error) => Err(error.into()),
        None => Ok(()),
    }
}

async fn exec(
    docker: &DockerClient,
    container: String,
    tty: bool,
    command: Vec<String>,
) -> anyhow::Result<ExitCode> {
    let created = docker
        .exec()
        .create(
            &container,
            NewExec {
                cmd: command,
                attach_stdin: true,
                attach_stdout: true,
                attach_stderr: true,
                tty,
            },
        )
        .await?;
    let tunnel = docker
        .exec()
        .start(&created.id, ExecStart { detach: false, tty })
        .await?;
    let framed = !tunnel.has_type(RAW_STREAM);
    let (mut from_command, mut to_command) = tokio::io::split(tunnel);

    // The command's input, until the command line's own ends. The half-close tells Docker, which closes the command's:
    // it comes whatever the copy gave, so that a command such as `cat` never waits for an input that has stopped.
    let sending = tokio::spawn(async move {
        let copied = tokio::io::copy(&mut tokio::io::stdin(), &mut to_command).await;
        let closed = to_command.shutdown().await;

        copied.map(drop).and(closed)
    });

    let mut demux = Demux::default();
    let mut stdout = tokio::io::stdout();
    let mut stderr = tokio::io::stderr();
    let mut chunk = vec![0; 8192];

    loop {
        let read = from_command.read(&mut chunk).await?;

        if read == 0 {
            break;
        }

        if !framed {
            stdout.write_all(&chunk[..read]).await?;
            stdout.flush().await?;
            continue;
        }

        write(demux.push(&chunk[..read]), &mut stdout, &mut stderr).await?;
    }

    demux.finish()?;

    // The command has ended: what is left of the input goes nowhere. An input that failed is said, the command's own
    // code still the one the command line exits with.
    if sending.is_finished() {
        if let Ok(Err(error)) = sending.await {
            eprintln!("warning: the command's input was not all sent: {error}");
        }
    } else {
        sending.abort();
    }

    // Docker sets the code before it closes the command's streams.
    let Some(code) = docker.exec().inspect(&created.id).await?.exit_code else {
        bail!("the command's streams ended, and Docker gives no exit code for it");
    };

    // A code that no process exits with, outside 0 to 255, is a failure all the same.
    Ok(u8::try_from(code).map_or(ExitCode::FAILURE, ExitCode::from))
}

async fn events(docker: &DockerClient, since: Option<i64>) -> anyhow::Result<()> {
    let mut events = docker.system().events(since).await?;

    while let Some(event) = events.next().await {
        let event = match event {
            Ok(event) => event,
            // An event this command line does not read, of a kind newer than it: the stream goes on.
            Err(error) if error.kind() == ErrorKind::Decode => {
                eprintln!("warning: an event is skipped: {}", error_chain(&error));
                continue;
            }
            Err(error) => return Err(error.into()),
        };
        let name = event
            .actor
            .attributes
            .get("name")
            .map_or("", String::as_str);

        println!(
            "{} {} {} {} {name}",
            event.time,
            event.kind,
            event.action,
            short_id(&event.actor.id)
        );
    }

    Ok(())
}
