//! The servers of `cargo quoll dev`: built, copied out of the target directory, started, and restarted on a newer
//! build, the one that runs stopped only once the new one has compiled.

use std::fs::{self, File, TryLockError};
use std::io;
use std::path::{Path, PathBuf};
use std::process::{self, ExitStatus};
use std::time::{Duration, SystemTime};

use anyhow::{Context, Result, anyhow};
use tokio::net::TcpStream;

use crate::cargo::Workspace;
use crate::commands;
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
/// replace an executable that runs. Each `cargo quoll dev` has a directory of its own, `quoll-dev/<pid>`, which it
/// holds locked while it runs.
#[derive(Debug)]
pub(crate) struct Copies {
    /// `quoll-dev` in the target directory.
    root: PathBuf,
    /// This one's directory in it.
    dir: PathBuf,
    /// The lock of the directory, which tells another `cargo quoll dev` that it is in use.
    lock: Option<File>,
    next: u64,
}

/// The file whose lock says a directory of copies is in use.
const LOCK: &str = "lock";

impl Copies {
    pub(crate) fn new(workspace: &Workspace) -> Self {
        let root = workspace.target_directory.join("quoll-dev");

        Self {
            dir: root.join(process::id().to_string()),
            root,
            lock: None,
            next: 0,
        }
    }

    /// Makes the directory of this `cargo quoll dev` and locks it, then removes those that another one left when it
    /// was killed, whose lock no one holds.
    pub(crate) async fn prepare(&mut self, reporter: &Reporter) {
        let root = self.root.clone();
        let dir = self.dir.clone();
        let prepared = tokio::task::spawn_blocking(move || prepare(&root, &dir)).await;

        match prepared {
            Ok((lock, warnings)) => {
                self.lock = lock;

                for warning in warnings {
                    reporter.status("Warning", warning);
                }
            }
            Err(error) => reporter.status("Warning", format!("cannot clear old copies: {error}")),
        }
    }

    /// Removes the directory of this `cargo quoll dev`, once its servers stopped.
    pub(crate) async fn finish(&mut self, reporter: &Reporter) {
        // Windows removes no file that is open.
        self.lock = None;
        remove(&self.dir, reporter).await;
    }

    /// Copies `executable` into a directory of its own, and returns the copy.
    async fn copy(&mut self, executable: &Path) -> Result<PathBuf> {
        let dir = self.dir.join(self.next.to_string());
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

/// Makes `dir` and locks it, then removes the other directories of `root` whose lock no one holds, and returns the
/// lock, none when it cannot be taken, with what could not be done.
fn prepare(root: &Path, dir: &Path) -> (Option<File>, Vec<String>) {
    let mut warnings = Vec::new();
    let lock = fs::create_dir_all(dir)
        .and_then(|()| File::create(dir.join(LOCK)))
        .and_then(|file| file.lock().map(|()| file));
    let lock = match lock {
        Ok(lock) => Some(lock),
        Err(error) => {
            warnings.push(format!("cannot lock {}: {error}", dir.display()));
            None
        }
    };

    let entries = match fs::read_dir(root) {
        Ok(entries) => entries,
        Err(error) => {
            warnings.push(format!("cannot read {}: {error}", root.display()));
            return (lock, warnings);
        }
    };

    for entry in entries.flatten() {
        let other = entry.path();

        if other == dir {
            continue;
        }

        let stale = match File::open(other.join(LOCK)) {
            // Its lock is taken, and let go as the file closes, before the directory is removed.
            Ok(file) => match file.try_lock() {
                Ok(()) => true,
                Err(TryLockError::WouldBlock) => false,
                Err(TryLockError::Error(error)) => {
                    warnings.push(format!("cannot lock {}: {error}", other.display()));
                    false
                }
            },
            // Copies with no lock are those of a `cargo quoll dev` that was killed as it started.
            Err(error) if error.kind() == io::ErrorKind::NotFound => true,
            Err(error) => {
                warnings.push(format!(
                    "cannot open the lock of {}: {error}",
                    other.display()
                ));
                false
            }
        };

        if stale
            && let Err(error) = fs::remove_dir_all(&other)
            && error.kind() != io::ErrorKind::NotFound
        {
            warnings.push(format!("cannot remove {}: {error}", other.display()));
        }
    }

    (lock, warnings)
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
            Ok(built) => Identity::of(&built.executable)
                .await
                .map(|identity| (built, identity)),
            Err(error) => Err(error),
        };
        let (built, identity) = match identity {
            Ok(found) => found,
            Err(error) => return self.failed(reporter, &error, None),
        };

        // Another cargo may have built the executable first, which this build then finds fresh: what tells is the
        // executable, not the build.
        if let Some(running) = &mut self.running
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

        let copy = match copies.copy(&built.executable).await {
            Ok(copy) => copy,
            Err(error) => return self.failed(reporter, &error, None),
        };

        self.stop(reporter).await;
        self.start(workspace, reporter, copy, &built, identity)
            .await;
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
        built: &Built,
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
                None,
            );
        }

        let mut command = process::Command::new(&copy);

        command
            .args(&self.config.args)
            .envs(self.config.env.iter().map(|(name, value)| (name, value)))
            .current_dir(&workspace.root);

        // The copy is out of the target directory: its dynamic libraries are found where `cargo run` would find
        // them, before those of the server's `env` or, without it, of `cargo quoll dev`.
        let inherited = self
            .config
            .env
            .iter()
            .find(|(name, _)| commands::is_variable(name, build::LIBRARY_PATH))
            .map(|(_, value)| value.into())
            .or_else(|| std::env::var_os(build::LIBRARY_PATH));
        let paths = built
            .library_paths
            .iter()
            .cloned()
            .chain(inherited.iter().flat_map(std::env::split_paths));

        match std::env::join_paths(paths) {
            Ok(joined) => {
                command.env(build::LIBRARY_PATH, joined);
            }
            Err(error) => reporter.status(
                "Warning",
                format!(
                    "cannot set {} for {}: {error}",
                    build::LIBRARY_PATH,
                    self.config.package
                ),
            ),
        }

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
                return self.failed(reporter, &error, None);
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
        match answered {
            Ok(true) => {
                reporter.status(
                    "Ready",
                    format!("{} on {}", self.config.package, self.config.url),
                );
                self.event(reporter, ServerState::Ready, None);
                self.running = Some(Running {
                    child,
                    copy: dir,
                    identity,
                });
            }
            Ok(false) => {
                let status = child.stop().await;

                remove(&dir, reporter).await;
                self.failed(
                    reporter,
                    &anyhow!(
                        "{} does not answer on {} after {} s: it is stopped",
                        self.config.package,
                        self.config.url,
                        READY.as_secs()
                    ),
                    child::code(&status),
                );
            }
            Err(status) => {
                drop(child);
                remove(&dir, reporter).await;

                let said = match &status {
                    Ok(status) => status.to_string(),
                    Err(error) => error.to_string(),
                };

                self.failed(
                    reporter,
                    &anyhow!(
                        "{} exited before it answered on {}, {said}",
                        self.config.package,
                        self.config.url
                    ),
                    child::code(&status),
                );
            }
        }
    }

    /// Reports a server that does not compile or cannot start, with the exit `code` of one that exited.
    fn failed(&self, reporter: &Reporter, error: &anyhow::Error, code: Option<i32>) {
        reporter.error(Some(&self.config.package), error);

        let left = if self.running.is_some() {
            "the last one that compiled keeps running"
        } else {
            "none runs"
        };

        reporter.status("Failed", format!("{}: {left}", self.config.package));
        self.event(reporter, ServerState::Failed, code);
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
