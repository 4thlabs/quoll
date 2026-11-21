//! What `cargo quoll dev` says: a line for a person on stderr, and with `--json` one JSON line per event on stdout.

use std::fmt::Display;
use std::io::{self, Write};
use std::process::Stdio;

use serde::Serialize;

use crate::cargo::Diagnostic;

/// Where the events go: to stderr for a person, and to stdout in JSON for a tool when `--json` asks for them.
#[derive(Debug, Clone, Copy)]
pub(crate) struct Reporter {
    json: bool,
}

/// An event of `--json`, one line of stdout.
#[derive(Debug, Serialize)]
#[serde(tag = "event", rename_all = "snake_case")]
pub(crate) enum Event<'a> {
    /// An api's document made: written when `changed`, left as it was otherwise.
    Document {
        package: &'a str,
        /// As the config writes it, from the workspace's root.
        path: &'a str,
        changed: bool,
    },
    /// Something failed: an error of the compiler, or of what `cargo quoll dev` runs.
    Error {
        source: Source,
        package: Option<&'a str>,
        message: &'a str,
        /// Where the compiler points, relative to the workspace's root.
        file: Option<&'a str>,
        line: Option<u64>,
        column: Option<u64>,
    },
    Server {
        state: ServerState,
        package: &'a str,
        url: &'a str,
        /// The exit code of a server that stopped, when it exited with one.
        #[serde(skip_serializing_if = "Option::is_none")]
        code: Option<i32>,
    },
    /// `web.dev`, started once the servers are ready.
    Web {
        state: WebState,
        /// The exit code of the command that stopped, when it exited with one.
        #[serde(skip_serializing_if = "Option::is_none")]
        code: Option<i32>,
    },
}

/// What failed: the compiler, or anything else.
#[derive(Debug, Clone, Copy, Serialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum Source {
    /// An error of rustc, as cargo gives it, with where it points.
    Cargo,
    /// A build that failed, a binary or a command that failed, a server that does not answer.
    Quoll,
}

#[derive(Debug, Clone, Copy, Serialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum ServerState {
    /// Cargo builds it, the server that runs keeping on.
    Building,
    /// It answers on its url: a new one, or the one that ran when the build changed nothing.
    Ready,
    /// It does not compile, or cannot start: the last one that compiled keeps running, when there is one.
    Failed,
    /// It stopped: for a newer build, at the end, or on its own.
    Stopped,
}

#[derive(Debug, Clone, Copy, Serialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum WebState {
    Started,
    Stopped,
}

impl Reporter {
    pub(crate) fn new(json: bool) -> Self {
        Self { json }
    }

    /// A line for a person, on stderr, as cargo writes its own: a verb, then what it is about.
    ///
    /// A line that cannot be written is left: `eprintln!` would panic on a terminal hung up or a pipe closed, and the
    /// panic would kill the children it is about to stop.
    pub(crate) fn status(&self, verb: &str, text: impl Display) {
        let _ = writeln!(io::stderr(), "{verb:>12} {text}");
    }

    /// The event in JSON, on stdout, with `--json`.
    pub(crate) fn event(&self, event: &Event<'_>) {
        if !self.json {
            return;
        }

        let mut stdout = io::stdout().lock();
        let written = serde_json::to_writer(&mut stdout, event)
            .map_err(io::Error::from)
            .and_then(|()| writeln!(stdout))
            .and_then(|()| stdout.flush());

        if let Err(error) = written {
            let _ = writeln!(
                io::stderr(),
                "error: cannot write an event on stdout: {error}"
            );
        }
    }

    /// An error of `cargo quoll dev`, printed, and its event.
    pub(crate) fn error(&self, package: Option<&str>, error: &anyhow::Error) {
        let message = format!("{error:#}");

        let _ = writeln!(io::stderr(), "error: {message}");
        self.event(&Event::Error {
            source: Source::Quoll,
            package,
            message: &message,
            file: None,
            line: None,
            column: None,
        });
    }

    /// A diagnostic of the compiler on `package`, printed as rustc renders it, and the event of an error.
    pub(crate) fn diagnostic(&self, package: &str, diagnostic: &Diagnostic) {
        if let Some(rendered) = &diagnostic.rendered {
            let _ = write!(io::stderr(), "{rendered}");
        }

        // rustc ends with an error that counts the others, which points at nothing.
        let summary =
            diagnostic.spans.is_empty() && diagnostic.message.starts_with("aborting due to");

        if diagnostic.level != "error" || summary {
            return;
        }

        let span = diagnostic.primary_span();

        self.event(&Event::Error {
            source: Source::Cargo,
            package: Some(package),
            message: &diagnostic.message,
            file: span.map(|span| span.file_name.as_str()),
            line: span.map(|span| span.line_start),
            column: span.map(|span| span.column_start),
        });
    }

    /// Where a child writes its stdout: to the terminal, or to stderr with `--json`, whose stdout holds the events
    /// alone.
    pub(crate) fn child_stdout(&self) -> io::Result<Stdio> {
        if !self.json {
            return Ok(Stdio::inherit());
        }

        stderr()
    }
}

#[cfg(unix)]
fn stderr() -> io::Result<Stdio> {
    use std::os::fd::AsFd;

    Ok(io::stderr().as_fd().try_clone_to_owned()?.into())
}

#[cfg(windows)]
fn stderr() -> io::Result<Stdio> {
    use std::os::windows::io::AsHandle;

    Ok(io::stderr().as_handle().try_clone_to_owned()?.into())
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn json(event: &Event<'_>) -> serde_json::Value {
        serde_json::to_value(event).expect("an event is JSON")
    }

    #[test]
    fn the_events_are_those_of_the_spec() {
        assert_eq!(
            json(&Event::Document {
                package: "notes-api",
                path: "web/openapi.json",
                changed: true,
            }),
            json!({ "event": "document", "package": "notes-api", "path": "web/openapi.json", "changed": true })
        );
        assert_eq!(
            json(&Event::Error {
                source: Source::Cargo,
                package: Some("notes-server"),
                message: "mismatched types",
                file: Some("server/src/main.rs"),
                line: Some(3),
                column: Some(5),
            }),
            json!({
                "event": "error",
                "source": "cargo",
                "package": "notes-server",
                "message": "mismatched types",
                "file": "server/src/main.rs",
                "line": 3,
                "column": 5,
            })
        );
        assert_eq!(
            json(&Event::Server {
                state: ServerState::Ready,
                package: "notes-server",
                url: "http://127.0.0.1:8080",
                code: None,
            }),
            json!({ "event": "server", "state": "ready", "package": "notes-server", "url": "http://127.0.0.1:8080" })
        );
        assert_eq!(
            json(&Event::Web {
                state: WebState::Stopped,
                code: Some(1),
            }),
            json!({ "event": "web", "state": "stopped", "code": 1 })
        );
    }
}
