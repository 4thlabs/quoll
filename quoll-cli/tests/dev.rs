//! `cargo quoll dev --json`, run as cargo runs it on the workspace under `tests/fixture`, its events read from its
//! stdout: a real server on a port, edited, broken and fixed, and every child gone once it stops.

#[path = "support/fixture.rs"]
mod fixture;

use std::collections::VecDeque;
use std::fs::{self, File};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::process::{Child, ExitStatus, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::thread;
use std::time::{Duration, Instant, SystemTime};

use fixture::Fixture;
use serde_json::Value;

/// How long a test waits for an event: a build of the fixture, cold on a slow runner.
const WAIT: Duration = Duration::from_secs(180);

/// The document of the contract, as the fixture writes it.
const DOCUMENT: &str = r#"{"openapi": "3.1.0"}"#;

/// The config of the tests: the contract's document, which `record` follows, its server on `{address}`, and
/// `web-dev`.
const CONFIG: &str = r#"
[[workspace.metadata.quoll.api]]
package = "contract"
bin = "openapi"
output = "web/openapi.json"
after = [["record", "after"]]

[[workspace.metadata.quoll.server]]
package = "server"
url = "http://{address}"

[workspace.metadata.quoll.web]
dir = "web"
dev = ["web-dev"]
"#;

/// A line of `cargo quoll dev`: an event of its stdout, a line of its stderr, or the end of either. Its stdout ends
/// once it exited; its stderr, which its children write to, once they all exited too.
enum Line {
    Event(Value),
    Stderr(String),
    Closed,
    StderrClosed,
}

/// `cargo quoll dev --json`, running on a fixture.
struct Dev {
    child: Child,
    lines: Receiver<Line>,
    /// The events read and not yet waited for.
    events: VecDeque<Value>,
    /// The lines of stderr read and not yet waited for.
    stderr: VecDeque<String>,
    /// All of stderr, for a test that fails.
    log: String,
    closed: bool,
    stderr_closed: bool,
    server: SocketAddr,
}

impl Dev {
    fn start(fixture: &Fixture) -> Self {
        let server = free_address();

        fs::write(
            fixture.path("Cargo.toml"),
            fs::read_to_string(fixture.path("Cargo.toml"))
                .expect("the workspace's Cargo.toml is read")
                + &CONFIG.replace("{address}", &server.to_string()),
        )
        .expect("the config is written");

        let mut child = fixture
            .command()
            .args(["dev", "--json"])
            .env("FIXTURE_SERVER_ADDRESS", server.to_string())
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("cargo-quoll runs");
        let (sender, lines) = mpsc::channel();
        let stdout = child.stdout.take().expect("stdout is piped");
        let stderr = child.stderr.take().expect("stderr is piped");
        let events = sender.clone();

        thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let line = line.expect("stdout is read");
                let event = serde_json::from_str(&line)
                    .unwrap_or_else(|error| panic!("`{line}` is no event: {error}"));

                if events.send(Line::Event(event)).is_err() {
                    return;
                }
            }

            let _ = events.send(Line::Closed);
        });
        thread::spawn(move || {
            for line in BufReader::new(stderr).lines() {
                let line = line.expect("stderr is read");

                if sender.send(Line::Stderr(line)).is_err() {
                    return;
                }
            }

            let _ = sender.send(Line::StderrClosed);
        });

        Self {
            child,
            lines,
            events: VecDeque::new(),
            stderr: VecDeque::new(),
            log: String::new(),
            closed: false,
            stderr_closed: false,
            server,
        }
    }

    /// Reads the next line, and fails the test after `WAIT`.
    #[track_caller]
    fn read(&mut self, deadline: Instant, waiting_for: &str) {
        let left = deadline.saturating_duration_since(Instant::now());

        match self.lines.recv_timeout(left) {
            Ok(Line::Event(event)) => self.events.push_back(event),
            Ok(Line::Stderr(line)) => {
                self.log.push_str(&line);
                self.log.push('\n');
                self.stderr.push_back(line);
            }
            Ok(Line::Closed) => self.closed = true,
            Ok(Line::StderrClosed) => self.stderr_closed = true,
            Err(RecvTimeoutError::Timeout) => {
                panic!("no {waiting_for} after {WAIT:?}; its stderr:\n{}", self.log)
            }
            Err(RecvTimeoutError::Disconnected) => {
                panic!(
                    "cargo quoll exited, with no {waiting_for}; its stderr:\n{}",
                    self.log
                )
            }
        }
    }

    /// Waits for the event that `matches`, and returns it with the events before it.
    #[track_caller]
    fn until(&mut self, what: &str, matches: impl Fn(&Value) -> bool) -> Vec<Value> {
        let deadline = Instant::now() + WAIT;
        let mut seen = Vec::new();

        loop {
            while let Some(event) = self.events.pop_front() {
                let found = matches(&event);

                seen.push(event);

                if found {
                    return seen;
                }
            }

            assert!(
                !self.closed,
                "cargo quoll exited, with no {what}; its events: {seen:?}; its stderr:\n{}",
                self.log
            );
            self.read(deadline, what);
        }
    }

    /// Waits for the line of stderr that starts with `start`, and returns the rest of it.
    #[track_caller]
    fn stderr_line(&mut self, start: &str) -> String {
        let deadline = Instant::now() + WAIT;

        loop {
            while let Some(line) = self.stderr.pop_front() {
                if let Some(rest) = line.strip_prefix(start) {
                    return rest.to_owned();
                }
            }

            self.read(deadline, &format!("`{start}` on stderr"));
        }
    }

    /// Waits for the server to be ready, and returns the events until then.
    #[track_caller]
    fn ready(&mut self) -> Vec<Value> {
        self.until("server ready", |event| is(event, "server", "ready"))
    }

    /// What the server answers.
    fn get(&self) -> String {
        let mut stream = TcpStream::connect(self.server).expect("the server answers");

        stream
            .set_read_timeout(Some(WAIT))
            .expect("the timeout is set");
        stream
            .write_all(b"GET / HTTP/1.0\r\n\r\n")
            .expect("the request is sent");

        let mut response = String::new();

        stream
            .read_to_string(&mut response)
            .expect("the response is read");

        let (_, body) = response
            .split_once("\r\n\r\n")
            .expect("the response has a body");

        body.to_owned()
    }

    /// Waits for `cargo quoll dev` to exit, once its stdout closed, and for all its children, once its stderr closed.
    #[track_caller]
    fn exit(&mut self) -> (ExitStatus, Vec<Value>) {
        let deadline = Instant::now() + WAIT;
        let mut events = Vec::new();

        while !self.closed || !self.stderr_closed {
            events.extend(self.events.drain(..));
            self.read(deadline, "exit");
        }

        events.extend(self.events.drain(..));

        (
            self.child.wait().expect("cargo quoll is waited for"),
            events,
        )
    }

    /// Sends `signal` to `cargo quoll dev`.
    #[cfg(unix)]
    fn signal(&self, signal: &str) {
        let status = std::process::Command::new("kill")
            .args([signal, &self.child.id().to_string()])
            .status()
            .expect("kill runs");

        assert!(status.success(), "the signal is sent");
    }
}

impl Drop for Dev {
    fn drop(&mut self) {
        if self.closed {
            return;
        }

        // A test that failed asks its children to stop, as a person would, so that none is left running. On
        // Windows, killing it is enough: its job takes them with it.
        #[cfg(unix)]
        {
            let _ = std::process::Command::new("kill")
                .args(["-TERM", &self.child.id().to_string()])
                .status();
            let deadline = Instant::now() + Duration::from_secs(30);

            while !self.closed && Instant::now() < deadline {
                match self
                    .lines
                    .recv_timeout(deadline.saturating_duration_since(Instant::now()))
                {
                    Ok(Line::Closed) => self.closed = true,
                    Ok(_) => {}
                    Err(_) => break,
                }
            }
        }

        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// An address on the loopback that nothing listens on.
fn free_address() -> SocketAddr {
    TcpListener::bind("127.0.0.1:0")
        .and_then(|listener| listener.local_addr())
        .expect("a port is free")
}

/// Whether `event` is an event `kind` in `state`.
fn is(event: &Value, kind: &str, state: &str) -> bool {
    event["event"] == kind && event["state"] == state
}

fn position(events: &[Value], matches: impl Fn(&Value) -> bool) -> usize {
    events
        .iter()
        .position(matches)
        .unwrap_or_else(|| panic!("no such event in {events:?}"))
}

#[track_caller]
fn assert_refused(address: SocketAddr, what: &str) {
    assert!(
        TcpStream::connect_timeout(&address, Duration::from_secs(10)).is_err(),
        "{what} still answers on {address}"
    );
}

/// The source of the server, with `version` for the one it answers.
fn with_version(source: &str, version: &str) -> String {
    source.replace("\"server 1\"", &format!("\"{version}\""))
}

fn record(fixture: &Fixture) -> String {
    fs::read_to_string(fixture.path("web/record.log")).unwrap_or_default()
}

fn modified(fixture: &Fixture) -> SystemTime {
    fs::metadata(fixture.path("web/openapi.json"))
        .and_then(|metadata| metadata.modified())
        .expect("the document's time is read")
}

/// Sets the document's time long ago, so that a write of it shows.
fn age_document(fixture: &Fixture) -> SystemTime {
    let long_ago = SystemTime::UNIX_EPOCH + Duration::from_secs(86_400);

    File::options()
        .write(true)
        .open(fixture.path("web/openapi.json"))
        .and_then(|file| file.set_modified(long_ago))
        .expect("the document's time is set");

    long_ago
}

#[test]
fn dev_writes_the_document_then_starts_the_server_then_web_dev() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);

    let events = dev.until("web started", |event| is(event, "web", "started"));

    let document = position(&events, |event| event["event"] == "document");
    let building = position(&events, |event| is(event, "server", "building"));
    let ready = position(&events, |event| is(event, "server", "ready"));
    assert!(document < building && building < ready, "{events:?}");
    assert_eq!(events[document]["package"], "contract");
    assert_eq!(events[document]["path"], "web/openapi.json");
    assert_eq!(events[document]["changed"], true);
    assert_eq!(events[ready]["package"], "server");
    assert_eq!(events[ready]["url"], format!("http://{}", dev.server));

    assert_eq!(dev.get(), format!("server 1 {DOCUMENT}"));
    assert_eq!(
        fs::read_to_string(fixture.path("web/openapi.json")).expect("the document is written"),
        format!("{DOCUMENT}\n")
    );
    assert_eq!(
        record(&fixture),
        "after\n",
        "after runs at the start, as gen"
    );
}

#[test]
fn a_change_of_the_server_restarts_it_alone() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);

    dev.ready();
    let long_ago = age_document(&fixture);

    let server = fixture.path("server/src/main.rs");
    let source = fs::read_to_string(&server).expect("the server is read");

    fs::write(&server, with_version(&source, "server 2")).expect("the server is written");
    let events = dev.ready();

    let building = position(&events, |event| is(event, "server", "building"));
    let stopped = position(&events, |event| is(event, "server", "stopped"));
    assert!(
        building < stopped,
        "the old server stops once the new one compiled: {events:?}"
    );
    assert!(
        !events.iter().any(|event| event["event"] == "document"),
        "no document is made: {events:?}"
    );
    assert_eq!(dev.get(), format!("server 2 {DOCUMENT}"));
    assert_eq!(modified(&fixture), long_ago, "the document is not written");
    assert_eq!(record(&fixture), "after\n");
}

#[test]
fn a_change_of_the_contract_writes_the_document_then_restarts_the_server() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);
    let contract = fixture.path("contract/src/lib.rs");
    let source = fs::read_to_string(&contract).expect("the contract is read");

    dev.ready();

    fs::write(&contract, source.replace("3.1.0", "3.1.1")).expect("the contract is written");
    let events = dev.ready();

    let document = position(&events, |event| event["event"] == "document");
    let building = position(&events, |event| is(event, "server", "building"));
    assert!(document < building, "{events:?}");
    assert_eq!(events[document]["changed"], true);
    assert_eq!(dev.get(), r#"server 1 {"openapi": "3.1.1"}"#);
    assert_eq!(
        fs::read_to_string(fixture.path("web/openapi.json")).expect("the document is written"),
        "{\"openapi\": \"3.1.1\"}\n"
    );
    assert_eq!(
        record(&fixture),
        "after\nafter\n",
        "after follows a new document"
    );

    // A change of the contract that leaves its document as it was.
    let long_ago = age_document(&fixture);

    fs::write(
        &contract,
        source.replace("3.1.0", "3.1.1")
            + "\n/// The same document.\npub const SAME: bool = true;\n",
    )
    .expect("the contract is written");
    let events = dev.ready();

    let document = position(&events, |event| event["event"] == "document");
    assert_eq!(events[document]["changed"], false);
    assert_eq!(
        modified(&fixture),
        long_ago,
        "the document is not written again"
    );
    assert_eq!(record(&fixture), "after\nafter\n", "after does not run");
}

#[test]
fn a_server_that_does_not_compile_leaves_the_last_one_running() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);
    let server = fixture.path("server/src/main.rs");
    let source = fs::read_to_string(&server).expect("the server is read");

    dev.ready();

    fs::write(&server, format!("{source}\nfn broken() {{ not_rust }}\n"))
        .expect("the server is broken");
    let events = dev.until("server failed", |event| is(event, "server", "failed"));

    let error = &events[position(&events, |event| {
        event["event"] == "error" && event["source"] == "cargo"
    })];
    assert_eq!(error["package"], "server");
    assert_eq!(
        error["message"],
        "cannot find value `not_rust` in this scope"
    );
    assert_eq!(
        error["file"]
            .as_str()
            .map(|file| file.replace('\\', "/"))
            .as_deref(),
        Some("server/src/main.rs")
    );
    assert!(
        error["line"].as_u64().is_some_and(|line| line > 1),
        "{error}"
    );
    assert!(error["column"].as_u64().is_some(), "{error}");
    assert!(
        !events.iter().any(|event| is(event, "server", "stopped")),
        "the old server keeps running: {events:?}"
    );
    assert_eq!(dev.get(), format!("server 1 {DOCUMENT}"));

    fs::write(&server, with_version(&source, "server 3")).expect("the server is fixed");
    dev.ready();

    assert_eq!(dev.get(), format!("server 3 {DOCUMENT}"));
}

/// Sends `signal` once everything runs, and checks that `cargo quoll dev` stopped the server and `web-dev`.
#[cfg(unix)]
fn stops_every_child_on(signal: &str) {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);

    dev.until("web started", |event| is(event, "web", "started"));
    let web: SocketAddr = dev
        .stderr_line("web-dev: listening on ")
        .parse()
        .expect("web-dev prints its address");

    dev.signal(signal);
    let (status, events) = dev.exit();

    assert!(status.success(), "{status}");
    assert!(
        events.iter().any(|event| is(event, "server", "stopped")),
        "{events:?}"
    );
    assert!(
        events.iter().any(|event| is(event, "web", "stopped")),
        "{events:?}"
    );
    assert_refused(dev.server, "the server");
    assert_refused(web, "web-dev");
}

#[cfg(unix)]
#[test]
fn ctrl_c_stops_every_child() {
    stops_every_child_on("-INT");
}

#[cfg(unix)]
#[test]
fn sigterm_stops_every_child() {
    stops_every_child_on("-TERM");
}

/// A Ctrl-C cannot be sent to another console's process from a test with no `unsafe`: `cargo quoll dev` is killed
/// instead, and its jobs take its children with them.
#[cfg(windows)]
#[test]
fn a_killed_cargo_quoll_leaves_no_child() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);

    dev.until("web started", |event| is(event, "web", "started"));
    let web: SocketAddr = dev
        .stderr_line("web-dev: listening on ")
        .parse()
        .expect("web-dev prints its address");

    dev.child.kill().expect("cargo quoll is killed");
    dev.exit();

    assert_refused(dev.server, "the server");
    assert_refused(web, "web-dev");
}
