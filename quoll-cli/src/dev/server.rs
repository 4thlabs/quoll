//! The servers of `cargo quoll dev`: built, copied out of the target directory, started, and restarted on a newer
//! build, the one that runs stopped only once the new one has compiled.

use std::io;
use std::path::{Path, PathBuf};
use std::process::{self, ExitStatus};
use std::time::Duration;

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
    ready: bool,
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
    pub(crate) async fn clear(&self) {
        // A copy that a stray server still runs cannot be removed on Windows: it stays, as it is no copy of ours.
        let _ = tokio::fs::remove_dir_all(&self.dir).await;
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

    /// Builds the server, then, when the build is new or none runs, stops the one that runs and starts the new one.
    /// A build that fails is reported, and leaves the one that runs.
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
        let Built { executable, fresh } = match built {
            Ok(built) => built,
            Err(error) => return self.failed(reporter, &error),
        };

        if fresh && self.is_ready() {
            reporter.status("Unchanged", &self.config.package);
            self.event(reporter, ServerState::Ready, None);
            return;
        }

        let copy = match copies.copy(&executable).await {
            Ok(copy) => copy,
            Err(error) => return self.failed(reporter, &error),
        };

        self.stop(reporter).await;
        self.start(workspace, reporter, copy).await;
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
    async fn start(&mut self, workspace: &Workspace, reporter: &Reporter, copy: PathBuf) {
        let dir = copy
            .parent()
            .expect("a copy is in a directory of its own")
            .to_path_buf();
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
            answered = tokio::time::timeout(READY, answers(&self.config.address)) => answered.is_ok(),
            status = child.wait() => {
                drop(child);
                remove(&dir, reporter).await;

                let said = match &status {
                    Ok(status) => status.to_string(),
                    Err(error) => error.to_string(),
                };

                reporter.error(
                    Some(&self.config.package),
                    &anyhow!("{} exited before it answered on {}, {said}", self.config.package, self.config.url),
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
