//! The servers of `cargo quoll dev`: built, copied out of the target directory, started, and restarted on a newer
//! build, the one that runs stopped only once the new one has compiled.

use std::io;
use std::path::{Path, PathBuf};
use std::process::{self, ExitStatus};
use std::time::{Duration, SystemTime};

use anyhow::{Context, Result, anyhow};
use tokio::net::TcpStream;

use crate::cargo::Workspace;
use crate::config;
use crate::dev::build::{self, Built};
use crate::dev::events::{Event, Reporter, ServerState};
use crate::dev::process::{self as child, Child};

/// How long a server has to answer once started.
const READY: Duration = Duration::from_secs(60);

/// How often a server that does not answer yet is tried again.
const RETRY: Duration = Duration::from_millis(50);

/// How long a connection to a server's url is waited for before it starts, to know whether another process holds its
/// port. A port no one holds refuses at once, but on Windows, which tries again for 2 s.
const HELD: Duration = Duration::from_millis(250);

/// A server of the config, and the process that runs it.
#[derive(Debug)]
pub(crate) struct Server<'a> {
    config: &'a config::Server,
    running: Option<Running>,
}

#[derive(Debug)]
struct Running {
    child: Child,
    /// The directory of the copy of its executable that runs.
    copy: PathBuf,
    /// The executable in the target directory it is a copy of, as it was then.
    identity: Identity,
    ready: bool,
}

/// What tells an executable from the next one cargo builds: its size and its time.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Identity {
    len: u64,
    modified: SystemTime,
}

impl Identity {
    async fn of(executable: &Path) -> Result<Self> {
        let metadata = tokio::fs::metadata(executable)
            .await
            .with_context(|| format!("cannot read {}", executable.display()))?;
        let modified = metadata
            .modified()
            .with_context(|| format!("cannot read the time of {}", executable.display()))?;

        Ok(Self {
            len: metadata.len(),
            modified,
        })
    }
}

/// Where the executables that run are copied, so that cargo can build the next one meanwhile: Windows does not
/// replace an executable that runs.
#[derive(Debug)]
pub(crate) struct Copies {
    dir: PathBuf,
    next: u64,
}

impl Copies {
    pub(crate) fn new(workspace: &Workspace) -> Self {
        Self {
            dir: workspace.target_directory.join("quoll-dev"),
            next: 0,
        }
    }

    /// Removes the copies an earlier `cargo quoll dev` left, when it was killed.
    pub(crate) async fn clear(&self, reporter: &Reporter) {
        match tokio::fs::remove_dir_all(&self.dir).await {
            Err(error) if error.kind() != io::ErrorKind::NotFound => reporter.status(
                "Warning",
                format!("cannot remove {}: {error}", self.dir.display()),
            ),
            _ => {}
        }
    }

    /// Copies `executable` into a directory of its own, and returns the copy.
    async fn copy(&mut self, executable: &Path) -> Result<PathBuf> {
        let dir = self.dir.join(format!("{}-{}", process::id(), self.next));
        let name = executable.file_name().context("an executable is a file")?;
        let copy = dir.join(name);

        self.next += 1;

        tokio::fs::create_dir_all(&dir)
            .await
            .with_context(|| format!("cannot make {}", dir.display()))?;
        tokio::fs::copy(executable, &copy).await.with_context(|| {
            format!("cannot copy {} to {}", executable.display(), copy.display())
        })?;

        Ok(copy)
    }
}

impl<'a> Server<'a> {
    pub(crate) fn new(config: &'a config::Server) -> Self {
        Self {
            config,
            running: None,
        }
    }

    pub(crate) fn package(&self) -> &str {
        &self.config.package
    }

    /// Whether the server runs and has answered.
    pub(crate) fn is_ready(&self) -> bool {
        self.running.as_ref().is_some_and(|running| running.ready)
    }

    /// The process that runs the server, when one does.
    pub(crate) fn child_mut(&mut self) -> Option<&mut Child> {
        self.running.as_mut().map(|running| &mut running.child)
    }

    /// Builds the server, then, unless the one that runs is a copy of the executable cargo gives and still runs, stops
    /// it and starts the new one. A build that fails is reported, and leaves the one that runs.
    pub(crate) async fn restart(
        &mut self,
        workspace: &Workspace,
        reporter: &Reporter,
        copies: &mut Copies,
    ) {
        let package = workspace.package(&self.config.package);

        reporter.status("Building", &self.config.package);
        self.event(reporter, ServerState::Building, None);

        let built = build::build(workspace, package, &self.config.bin, reporter).await;
        let identity = match built {
            Ok(Built { executable }) => Identity::of(&executable)
                .await
                .map(|identity| (executable, identity)),
            Err(error) => Err(error),
        };
        let (executable, identity) = match identity {
            Ok(found) => found,
            Err(error) => return self.failed(reporter, &error),
        };

        // Another cargo may have built the executable first, which this build then finds fresh: what tells is the
        // executable, not the build.
        if let Some(running) = &mut self.running
            && running.ready
            && running.identity == identity
            && running
                .child
                .try_wait()
                .is_ok_and(|status| status.is_none())
        {
            reporter.status("Unchanged", &self.config.package);
            self.event(reporter, ServerState::Ready, None);
            return;
        }

        let copy = match copies.copy(&executable).await {
            Ok(copy) => copy,
            Err(error) => return self.failed(reporter, &error),
        };

        self.stop(reporter).await;
        self.start(workspace, reporter, copy, identity).await;
    }

    /// Stops the server that runs, when one does.
    pub(crate) async fn stop(&mut self, reporter: &Reporter) {
        let Some(running) = self.running.take() else {
            return;
        };

        let status = running.child.stop().await;

        reporter.status("Stopped", &self.config.package);
        self.event(reporter, ServerState::Stopped, child::code(&status));
        remove(&running.copy, reporter).await;
    }

    /// The server that runs exited on its own, with `status`: it is started again by the next change.
    pub(crate) async fn exited(&mut self, status: io::Result<ExitStatus>, reporter: &Reporter) {
        let Some(running) = self.running.take() else {
            return;
        };

        drop(running.child);

        let said = match &status {
            Ok(status) => status.to_string(),
            Err(error) => error.to_string(),
        };

        reporter.status(
            "Stopped",
            format!("{}, {said}: a change starts it again", self.config.package),
        );
        self.event(reporter, ServerState::Stopped, child::code(&status));
        remove(&running.copy, reporter).await;
    }

    /// Starts `copy`, and waits for it to answer on the server's url, or to exit.
    async fn start(
        &mut self,
        workspace: &Workspace,
        reporter: &Reporter,
        copy: PathBuf,
        identity: Identity,
    ) {
        let dir = copy
            .parent()
            .expect("a copy is in a directory of its own")
            .to_path_buf();

        // A server that cannot listen would exit, but only once it started, while the process that holds its port
        // answers for it.
        if tokio::time::timeout(HELD, TcpStream::connect(&self.config.address))
            .await
            .is_ok_and(|connected| connected.is_ok())
        {
            remove(&dir, reporter).await;
            return self.failed(
                reporter,
                &anyhow!(
                    "another process answers on {}: the server cannot listen there",
                    self.config.url
                ),
            );
        }

        let mut command = process::Command::new(&copy);

        command.current_dir(&workspace.root);

        let spawned = reporter
            .child_stdout()
            .and_then(|stdout| {
                command.stdout(stdout);
                Child::spawn(command)
            })
            .with_context(|| format!("cannot start {}", copy.display()));
        let mut child = match spawned {
            Ok(child) => child,
            Err(error) => {
                remove(&dir, reporter).await;
                return self.failed(reporter, &error);
            }
        };

        reporter.status(
            "Started",
            format!("{} on {}", self.config.package, self.config.url),
        );

        let answered = tokio::select! {
            answered = tokio::time::timeout(READY, answers(&self.config.address)) => Ok(answered.is_ok()),
            status = child.wait() => Err(status),
        };
        // A process that held the port since the check, rather than the server, may have answered.
        let answered = match answered {
            Ok(true) => match child.try_wait() {
                Ok(Some(status)) => Err(Ok(status)),
                _ => Ok(true),
            },
            other => other,
        };
        let answered = match answered {
            Ok(answered) => answered,
            Err(status) => {
                drop(child);
                remove(&dir, reporter).await;

                let said = match &status {
                    Ok(status) => status.to_string(),
                    Err(error) => error.to_string(),
                };

                reporter.error(
                    Some(&self.config.package),
                    &anyhow!(
                        "{} exited before it answered on {}, {said}",
                        self.config.package,
                        self.config.url
                    ),
                );
                self.event(reporter, ServerState::Stopped, child::code(&status));
                return;
            }
        };

        if answered {
            reporter.status(
                "Ready",
                format!("{} on {}", self.config.package, self.config.url),
            );
            self.event(reporter, ServerState::Ready, None);
        } else {
            reporter.error(
                Some(&self.config.package),
                &anyhow!(
                    "{} does not answer on {} after {} s: it keeps running",
                    self.config.package,
                    self.config.url,
                    READY.as_secs()
                ),
            );
        }

        self.running = Some(Running {
            child,
            copy: dir,
            identity,
            ready: answered,
        });
    }

    fn failed(&self, reporter: &Reporter, error: &anyhow::Error) {
        reporter.error(Some(&self.config.package), error);

        let left = if self.running.is_some() {
            "the last one that compiled keeps running"
        } else {
            "none runs"
        };

        reporter.status("Failed", format!("{}: {left}", self.config.package));
        self.event(reporter, ServerState::Failed, None);
    }

    fn event(&self, reporter: &Reporter, state: ServerState, code: Option<i32>) {
        reporter.event(&Event::Server {
            state,
            package: &self.config.package,
            url: &self.config.url,
            code,
        });
    }
}

/// Returns once `address` accepts a connection.
async fn answers(address: &str) {
    loop {
        if TcpStream::connect(address).await.is_ok() {
            return;
        }

        tokio::time::sleep(RETRY).await;
    }
}

/// Removes the directory of a copy, whose server stopped.
async fn remove(dir: &Path, reporter: &Reporter) {
    if let Err(error) = tokio::fs::remove_dir_all(dir).await {
        reporter.status(
            "Warning",
            format!("cannot remove {}: {error}", dir.display()),
        );
    }
}
