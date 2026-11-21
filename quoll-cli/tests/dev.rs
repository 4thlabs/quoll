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
        Self::start_with(fixture, free_address(), |config| config)
    }

    /// `cargo quoll dev` with its server on `server`, and the config `edit` makes of the tests' one.
    fn start_with(fixture: &Fixture, server: SocketAddr, edit: impl Fn(String) -> String) -> Self {
        let config = edit(CONFIG.replace("{address}", &server.to_string()));

        fs::write(
            fixture.path("Cargo.toml"),
            fs::read_to_string(fixture.path("Cargo.toml"))
                .expect("the workspace's Cargo.toml is read")
                + &config,
        )
        .expect("the config is written");

        Self::spawn(fixture, server, &[])
    }

    /// `cargo quoll dev` on the config the fixture has, its server on `server`, with `envs` set.
    fn spawn(fixture: &Fixture, server: SocketAddr, envs: &[(&str, &str)]) -> Self {
        let mut child = fixture
            .command()
            .args(["dev", "--json"])
            .env("FIXTURE_SERVER_ADDRESS", server.to_string())
            .envs(envs.iter().copied())
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

#[test]
fn a_web_side_at_the_workspace_s_root_leaves_the_packages_watched() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start_with(&fixture, free_address(), |config| {
        config.replace("dir = \"web\"", "dir = \".\"")
    });
    let server = fixture.path("server/src/main.rs");
    let source = fs::read_to_string(&server).expect("the server is read");

    dev.ready();

    fs::write(&server, with_version(&source, "server 2")).expect("the server is written");
    dev.ready();

    assert_eq!(dev.get(), format!("server 2 {DOCUMENT}"));
}

#[test]
fn a_directory_made_after_the_start_is_watched() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);

    dev.ready();

    fs::create_dir_all(fixture.path("server/assets/icons")).expect("the directory is made");
    dev.ready();

    fs::write(fixture.path("server/assets/icons/logo.svg"), "<svg/>").expect("the file is written");
    let events = dev.ready();

    position(&events, |event| is(event, "server", "building"));
}

#[test]
fn a_document_written_inside_its_contract_starts_no_round() {
    let fixture = Fixture::new("");

    // No `after`, which would write its own files beside the document, in the contract.
    fs::create_dir(fixture.path("web")).expect("the directory is made");
    let mut dev = Dev::start_with(&fixture, free_address(), |config| {
        config
            .replace(
                "output = \"web/openapi.json\"",
                "output = \"contract/openapi.json\"",
            )
            .replace("after = [[\"record\", \"after\"]]\n", "")
    });
    let server = fixture.path("server/src/main.rs");
    let source = fs::read_to_string(&server).expect("the server is read");

    dev.ready();

    fs::write(&server, with_version(&source, "server 2")).expect("the server is written");
    let events = dev.ready();

    assert!(
        !events.iter().any(|event| event["event"] == "document"),
        "the document's first write is no change of its contract: {events:?}"
    );
    assert_eq!(dev.get(), format!("server 2 {DOCUMENT}"));
}

#[test]
fn a_server_built_by_another_cargo_is_restarted() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);
    let server = fixture.path("server/src/main.rs");
    let source = fs::read_to_string(&server).expect("the server is read");

    dev.ready();

    // The edit, then a build before the one of `cargo quoll dev`, which waits for its lock and finds it fresh.
    fs::write(&server, with_version(&source, "server 2")).expect("the server is written");
    let status =
        std::process::Command::new(std::env::var_os("CARGO").unwrap_or_else(|| "cargo".into()))
            .args(["build", "--quiet", "--package", "server"])
            .current_dir(fixture.path(""))
            .env_remove("CARGO_TARGET_DIR")
            .env_remove("CARGO_BUILD_TARGET_DIR")
            .status()
            .expect("cargo runs");
    assert!(status.success(), "the server builds");
    dev.ready();

    assert_eq!(dev.get(), format!("server 2 {DOCUMENT}"));
}

#[test]
fn after_runs_once_its_contract_is_fixed_though_the_document_did_not_change() {
    let fixture = Fixture::new("");
    let openapi = fixture.path("contract/src/bin/openapi.rs");
    let source = fs::read_to_string(&openapi).expect("the contract's binary is read");

    // A committed document, and a contract that does not compile at the start.
    fs::create_dir(fixture.path("web")).expect("the directory is made");
    fs::write(fixture.path("web/openapi.json"), format!("{DOCUMENT}\n"))
        .expect("the document is written");
    fs::write(&openapi, "fn main() { not_rust }\n").expect("the binary is broken");

    let mut dev = Dev::start(&fixture);
    let events = dev.until("web started", |event| is(event, "web", "started"));

    assert!(
        events
            .iter()
            .any(|event| event["event"] == "error" && event["package"] == "contract"),
        "{events:?}"
    );
    assert_eq!(
        record(&fixture),
        "",
        "after does not run without its document"
    );

    fs::write(&openapi, &source).expect("the binary is fixed");
    let events = dev.ready();

    let document = position(&events, |event| event["event"] == "document");
    assert_eq!(events[document]["changed"], false);
    assert_eq!(
        record(&fixture),
        "after\n",
        "after runs, which it could not at the start"
    );
}

#[test]
fn a_port_held_by_another_process_fails_the_server() {
    let fixture = Fixture::new("");
    let stray = TcpListener::bind("127.0.0.1:0").expect("a port is free");
    let address = stray.local_addr().expect("the listener has an address");
    let mut dev = Dev::start_with(&fixture, address, |config| config);

    let events = dev.until("server failed", |event| is(event, "server", "failed"));

    assert!(
        !events.iter().any(|event| is(event, "server", "ready")),
        "the stray process is no server: {events:?}"
    );
    let error = &events[position(&events, |event| event["event"] == "error")];
    assert!(
        error["message"]
            .as_str()
            .is_some_and(|message| message.contains("another process answers")),
        "{error}"
    );
}

/// Writes the config of the tests, its server on `server`, as `edit` makes it.
fn configure(fixture: &Fixture, server: SocketAddr, edit: impl Fn(String) -> String) {
    let config = edit(CONFIG.replace("{address}", &server.to_string()));

    fs::write(
        fixture.path("Cargo.toml"),
        fs::read_to_string(fixture.path("Cargo.toml")).expect("the workspace's Cargo.toml is read")
            + &config,
    )
    .expect("the config is written");
}

#[test]
fn a_directory_removed_and_made_again_is_watched() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);
    let assets = fixture.path("server/assets");

    dev.ready();

    fs::create_dir(&assets).expect("the directory is made");
    dev.ready();

    // Removed and made again within the same gathering of events.
    fs::remove_dir(&assets).expect("the directory is removed");
    fs::create_dir(&assets).expect("the directory is made again");
    dev.ready();

    fs::write(assets.join("logo.svg"), "<svg/>").expect("the file is written");
    let events = dev.ready();

    position(&events, |event| is(event, "server", "building"));
}

#[test]
fn a_change_of_the_cargo_lock_makes_everything_again() {
    let fixture = Fixture::new("");
    let mut dev = Dev::start(&fixture);
    let lock = fixture.path("Cargo.lock");

    dev.ready();

    let content = fs::read(&lock).expect("the Cargo.lock is read");
    fs::write(&lock, content).expect("the Cargo.lock is written");
    let events = dev.ready();

    let document = position(&events, |event| event["event"] == "document");
    let building = position(&events, |event| is(event, "server", "building"));
    assert!(document < building, "{events:?}");
}

#[test]
fn a_path_dependency_outside_the_workspace_is_watched() {
    let fixture = Fixture::new("");
    let shared = tempfile::tempdir().expect("a temporary directory");

    fs::create_dir(shared.path().join("src")).expect("the directory is made");
    fs::write(
        shared.path().join("Cargo.toml"),
        "[package]\nname = \"shared\"\nedition = \"2024\"\npublish = false\n",
    )
    .expect("the manifest is written");
    fs::write(
        shared.path().join("src/lib.rs"),
        "//! Shared.\n\n/// Its name.\npub const NAME: &str = \"shared 1\";\n",
    )
    .expect("the crate is written");

    let manifest = fixture.path("server/Cargo.toml");
    let path = shared.path().to_string_lossy().replace('\\', "/");
    let server = fixture.path("server/src/main.rs");

    fs::write(
        &manifest,
        fs::read_to_string(&manifest).expect("the manifest is read")
            + &format!("shared = {{ path = \"{path}\" }}\n"),
    )
    .expect("the manifest is written");
    fs::write(
        &server,
        fs::read_to_string(&server)
            .expect("the server is read")
            .replace(
                "format!(\"{VERSION} {}\", contract::DOCUMENT)",
                "format!(\"{VERSION} {} {}\", contract::DOCUMENT, shared::NAME)",
            ),
    )
    .expect("the server is written");

    let mut dev = Dev::start(&fixture);

    dev.ready();
    assert_eq!(dev.get(), format!("server 1 {DOCUMENT} shared 1"));

    fs::write(
        shared.path().join("src/lib.rs"),
        "//! Shared.\n\n/// Its name.\npub const NAME: &str = \"shared 2\";\n",
    )
    .expect("the crate is written");
    dev.ready();

    assert_eq!(dev.get(), format!("server 1 {DOCUMENT} shared 2"));
}

#[test]
fn a_server_that_exits_before_it_answers_fails_and_web_dev_starts() {
    let fixture = Fixture::new("");
    let server = free_address();

    configure(&fixture, server, |config| config);
    let mut dev = Dev::spawn(&fixture, server, &[("FIXTURE_SERVER_EXIT", "3")]);

    let events = dev.until("web started", |event| is(event, "web", "started"));

    let failed = &events[position(&events, |event| is(event, "server", "failed"))];
    assert_eq!(failed["code"], 3, "{failed}");
    assert!(
        !events.iter().any(|event| is(event, "server", "stopped")),
        "{events:?}"
    );
}

#[test]
fn a_second_dev_leaves_the_copies_of_the_first() {
    let fixture = Fixture::new("");
    let mut first = Dev::start(&fixture);

    first.ready();

    let copies = fixture.path(&format!("target/quoll-dev/{}/0", first.child.id()));
    assert!(copies.is_dir(), "{} is there", copies.display());

    // The second one finds the server's port held, by the first one's server.
    let mut second = Dev::spawn(&fixture, first.server, &[]);

    second.until("server failed", |event| is(event, "server", "failed"));

    assert!(copies.is_dir(), "{} is left", copies.display());
    assert_eq!(first.get(), format!("server 1 {DOCUMENT}"));
}

/// A child's script for the tests: says it runs, then, asked to stop, takes its time and writes `stopped.txt`
/// before it exits.
#[cfg(unix)]
const TRAPPING: &str = "trap 'sleep 0.3; echo stopped > stopped.txt; exit 0' TERM\n\
                        echo 'child: trapping'\n\
                        while :; do sleep 0.05; done\n";

#[cfg(unix)]
#[test]
fn what_web_dev_started_is_stopped_before_it_is_killed() {
    let fixture = Fixture::new("");

    fs::create_dir(fixture.path("web")).expect("the directory is made");
    fs::write(fixture.path("web/child.sh"), TRAPPING).expect("the script is written");
    let mut dev = Dev::start_with(&fixture, free_address(), |config| {
        config.replace(
            "dev = [\"web-dev\"]",
            "dev = [\"sh\", \"-c\", \"sh child.sh & wait\"]",
        )
    });

    dev.until("web started", |event| is(event, "web", "started"));
    dev.stderr_line("child: trapping");

    dev.signal("-INT");
    dev.exit();

    assert_eq!(
        fs::read_to_string(fixture.path("web/stopped.txt"))
            .ok()
            .as_deref(),
        Some("stopped\n"),
        "the child of the shell stopped as asked"
    );
}

#[cfg(unix)]
#[test]
fn a_signal_during_a_round_stops_its_children_as_asked() {
    let fixture = Fixture::new("");

    fs::create_dir(fixture.path("web")).expect("the directory is made");
    fs::write(fixture.path("web/child.sh"), TRAPPING).expect("the script is written");
    let mut dev = Dev::start_with(&fixture, free_address(), |config| {
        config.replace(
            "after = [[\"record\", \"after\"]]",
            "after = [[\"sh\", \"child.sh\"]]",
        )
    });

    dev.stderr_line("child: trapping");

    dev.signal("-INT");
    let (status, _) = dev.exit();

    assert!(status.success(), "{status}");
    assert_eq!(
        fs::read_to_string(fixture.path("web/stopped.txt"))
            .ok()
            .as_deref(),
        Some("stopped\n"),
        "the after cut short stopped as asked"
    );
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
