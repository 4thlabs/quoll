//! `cargo quoll dev`: `gen`, the servers and `web.dev` started, then the documents and the servers a change touches
//! made again, until Ctrl-C or `SIGTERM` stops every child.

mod build;
mod events;
mod process;
mod server;
mod watch;

use std::collections::BTreeSet;
use std::future::{Future, poll_fn};
use std::io;
use std::path::Path;
use std::pin::Pin;
use std::process::{ExitStatus, Stdio};
use std::task::Poll;

use anyhow::{Context, Result, anyhow, bail};
use tokio::io::AsyncReadExt;
use tokio::sync::mpsc;

use crate::cargo::Workspace;
use crate::commands::Command;
use crate::config::Api;
use crate::dev::events::{Event, Reporter, WebState};
use crate::dev::process::Child;
use crate::dev::server::{Copies, Server};
use crate::dev::watch::{Graph, Watched};
use crate::documents;

/// Runs the dev loop until it is asked to stop, then stops every child. `json` prints its events on stdout.
pub(crate) fn run(workspace: &Workspace, json: bool) -> Result<()> {
    let reporter = Reporter::new(json);
    let graph = Graph::new(workspace);
    let config = &workspace.config;
    let roots: Vec<_> = config
        .apis
        .iter()
        .map(|api| api.package.as_str())
        .chain(config.servers.iter().map(|server| server.package.as_str()))
        .map(|package| graph.index(package))
        .collect();
    let (sender, receiver) = mpsc::unbounded_channel();
    // Watching starts before the first round, so that a change made meanwhile is seen. It reads the file system, as
    // the graph does, before the runtime starts.
    let (_watcher, watched) = watch::start(workspace, graph.clone(), roots, sender, &reporter)?;

    reporter.status("Watching", format!("{watched} package(s)"));

    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .context("cannot start the runtime of `cargo quoll dev`")?;

    runtime.block_on(Dev::new(workspace, reporter, graph).run(receiver))
}

/// The documents to make again in a round, and the servers to restart: those a change touches.
#[derive(Debug)]
enum Changed {
    /// The first round, `gen` and every server.
    All,
    /// The packages whose files changed, by their index in the workspace.
    Packages(BTreeSet<usize>),
}

/// A child that exited on its own.
enum Exit {
    Server(usize, io::Result<ExitStatus>),
    Web(io::Result<ExitStatus>),
}

struct Dev<'a> {
    workspace: &'a Workspace,
    reporter: Reporter,
    graph: Graph,
    servers: Vec<Server<'a>>,
    /// `web.dev` once it runs: it starts once, and its bundler watches its own files.
    web: Option<Child>,
    copies: Copies,
    /// For each api, whether its `after` has to run, though its document did not change: at the start, as `gen`
    /// runs it, and until every command of a run of it succeeded, so that a document made while it could not run
    /// still gets its TypeScript.
    after_pending: Vec<bool>,
}

impl<'a> Dev<'a> {
    fn new(workspace: &'a Workspace, reporter: Reporter, graph: Graph) -> Self {
        Self {
            workspace,
            reporter,
            graph,
            servers: workspace.config.servers.iter().map(Server::new).collect(),
            web: None,
            copies: Copies::new(workspace),
            after_pending: vec![true; workspace.config.apis.len()],
        }
    }

    async fn run(mut self, mut receiver: mpsc::UnboundedReceiver<Watched>) -> Result<()> {
        let mut shutdown =
            Shutdown::new().context("cannot listen to the signals that stop `cargo quoll dev`")?;

        self.copies.prepare(&self.reporter).await;

        let first = async {
            self.round(&Changed::All).await;
            self.start_web().await;
        };
        let started = tokio::select! {
            () = first => true,
            () = shutdown.requested() => false,
        };
        let result = if started {
            self.watch(&mut receiver, &mut shutdown).await
        } else {
            Ok(())
        };

        self.reporter.status("Stopping", "every child");

        // The children of a round a signal cut short are stopped as they are dropped, then waited for here. A second
        // Ctrl-C does not wait: they are killed as the runtime drops their stops.
        let stopping = async {
            self.stop().await;
            process::stopped().await;
        };

        tokio::select! {
            () = stopping => {}
            () = shutdown.requested() => {}
        }

        self.copies.finish(&self.reporter).await;

        result
    }

    /// Makes again what each change touches, until a signal asks to stop.
    async fn watch(
        &mut self,
        receiver: &mut mpsc::UnboundedReceiver<Watched>,
        shutdown: &mut Shutdown,
    ) -> Result<()> {
        loop {
            tokio::select! {
                () = shutdown.requested() => return Ok(()),
                exit = exited(&mut self.servers, self.web.as_mut()) => self.exited(exit).await,
                watched = receiver.recv() => {
                    let Some(watched) = watched else {
                        bail!("the watcher of the workspace stopped");
                    };

                    let mut changed = BTreeSet::new();

                    self.take(watched, &mut changed);

                    // The changes made during the last round, gathered into this one.
                    while let Ok(watched) = receiver.try_recv() {
                        self.take(watched, &mut changed);
                    }

                    if changed.is_empty() {
                        continue;
                    }

                    let names: Vec<_> = changed.iter().map(|&package| self.graph.name(package)).collect();

                    self.reporter.status("Changed", names.join(", "));

                    let changed = Changed::Packages(changed);

                    tokio::select! {
                        () = self.round(&changed) => {}
                        () = shutdown.requested() => return Ok(()),
                    }
                }
            }
        }
    }

    fn take(&self, watched: Watched, changed: &mut BTreeSet<usize>) {
        match watched {
            Watched::Changed(packages) => changed.extend(packages),
            Watched::Error(error) => self
                .reporter
                .error(None, &anyhow!("the watcher of the workspace: {error}")),
        }
    }

    /// Makes the documents `changed` touches, then restarts the servers it touches.
    async fn round(&mut self, changed: &Changed) {
        let workspace = self.workspace;

        for (index, api) in workspace.config.apis.iter().enumerate() {
            if !self.touches(changed, &api.package) {
                continue;
            }

            let pending = &mut self.after_pending[index];

            if let Err(error) = document(workspace, &self.reporter, api, pending).await {
                self.reporter.error(Some(&api.package), &error);
            }
        }

        for index in 0..self.servers.len() {
            if self.touches(changed, self.servers[index].package()) {
                self.servers[index]
                    .restart(workspace, &self.reporter, &mut self.copies)
                    .await;
            }
        }
    }

    /// Whether `changed` touches `package`: a change of it or of a package it is built with.
    fn touches(&self, changed: &Changed, package: &str) -> bool {
        let Changed::Packages(packages) = changed else {
            return true;
        };
        let package = self.graph.index(package);

        packages
            .iter()
            .any(|&changed| self.graph.depends_on(package, changed))
    }

    /// Starts `web.dev`, once the first round has given each server its state, ready or failed.
    async fn start_web(&mut self) {
        let Some(web) = &self.workspace.config.web else {
            return;
        };
        let Some(dev) = &web.dev else {
            return;
        };

        match spawn(dev, &self.workspace.root.join(&web.dir), &self.reporter).await {
            Ok(child) => {
                self.web = Some(child);
                self.reporter.event(&Event::Web {
                    state: WebState::Started,
                    code: None,
                });
            }
            Err(error) => self.reporter.error(None, &error),
        }
    }

    async fn exited(&mut self, exit: Exit) {
        match exit {
            Exit::Server(index, status) => self.servers[index].exited(status, &self.reporter).await,
            Exit::Web(status) => {
                self.web = None;
                self.web_stopped(&status);
            }
        }
    }

    /// Stops `web.dev`, then the servers.
    async fn stop(&mut self) {
        if let Some(web) = self.web.take() {
            let status = web.stop().await;

            self.web_stopped(&status);
        }

        for server in &mut self.servers {
            server.stop(&self.reporter).await;
        }
    }

    fn web_stopped(&self, status: &io::Result<ExitStatus>) {
        let said = match status {
            Ok(status) => status.to_string(),
            Err(error) => error.to_string(),
        };

        self.reporter.status("Stopped", format!("web.dev, {said}"));
        self.reporter.event(&Event::Web {
            state: WebState::Stopped,
            code: process::code(status),
        });
    }
}

/// Waits for the first of the servers and `web.dev` to exit, forever when none runs.
async fn exited(servers: &mut [Server<'_>], web: Option<&mut Child>) -> Exit {
    let mut waits: Vec<Pin<Box<dyn Future<Output = Exit> + '_>>> = Vec::new();

    for (index, server) in servers.iter_mut().enumerate() {
        if let Some(child) = server.child_mut() {
            waits.push(Box::pin(
                async move { Exit::Server(index, child.wait().await) },
            ));
        }
    }

    if let Some(web) = web {
        waits.push(Box::pin(async move { Exit::Web(web.wait().await) }));
    }

    poll_fn(|cx| {
        waits
            .iter_mut()
            .find_map(|wait| match wait.as_mut().poll(cx) {
                Poll::Ready(exit) => Some(exit),
                Poll::Pending => None,
            })
            .map_or(Poll::Pending, Poll::Ready)
    })
    .await
}

/// Makes `api`'s document, written when its content changed, then runs its `after` commands when it changed or
/// `after_pending` says they have to run. `after_pending` is left set until every command succeeded.
async fn document(
    workspace: &Workspace,
    reporter: &Reporter,
    api: &Api,
    after_pending: &mut bool,
) -> Result<()> {
    let package = workspace.package(&api.package);
    let built = build::build(workspace, package, &api.bin, reporter)
        .await
        .with_context(|| api.key.clone())?;
    let mut command = std::process::Command::new(&built.executable);

    command
        .current_dir(&workspace.root)
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit());

    let mut child = Child::spawn(command)
        .with_context(|| format!("{}: cannot run {}", api.key, built.executable.display()))?;
    let mut document = Vec::new();

    child
        .stdout()
        .expect("the binary's stdout is piped")
        .read_to_end(&mut document)
        .await
        .with_context(|| format!("{}: cannot read the document", api.key))?;

    let status = child.wait().await.with_context(|| {
        format!(
            "{}: cannot wait for {}",
            api.key,
            built.executable.display()
        )
    })?;

    if !status.success() {
        bail!(
            "{}: the binary `{}` of `{}` failed, {status}",
            api.key,
            api.bin,
            api.package
        );
    }

    let path = workspace.root.join(&api.output);
    let written = path.clone();
    let changed = tokio::task::spawn_blocking(move || documents::write(&written, &document))
        .await
        .context("the write of the document panicked")?
        .with_context(|| format!("{}: cannot write it", documents::output(api)))?;

    reporter.status(if changed { "Wrote" } else { "Unchanged" }, &api.output);
    reporter.event(&Event::Document {
        package: &api.package,
        path: &api.output,
        changed,
    });

    *after_pending |= changed;

    if !*after_pending {
        return Ok(());
    }

    let dir = path.parent().unwrap_or(&workspace.root);

    for command in &api.after {
        let mut child = spawn(command, dir, reporter).await?;
        let status = child
            .wait()
            .await
            .with_context(|| format!("{}: cannot wait for `{command}`", command.key()))?;

        command.check(status)?;
    }

    *after_pending = false;

    Ok(())
}

/// Starts `command` in `dir`, its stdout where the reporter says. Its program is found on the PATH apart from the
/// runtime, as it reads the file system.
async fn spawn(command: &Command, dir: &Path, reporter: &Reporter) -> Result<Child> {
    let found = command.clone();
    let in_dir = dir.to_path_buf();
    let mut process = tokio::task::spawn_blocking(move || found.process(&in_dir))
        .await
        .context("the search of a program panicked")??;

    process.stdout(
        reporter
            .child_stdout()
            .context("cannot pass stderr to a child")?,
    );

    Child::spawn(process).with_context(|| format!("{}: cannot start `{command}`", command.key()))
}

/// The signals that stop `cargo quoll dev`: Ctrl-C, and `SIGTERM` and `SIGHUP` on Unix, Ctrl-Break and the console
/// closing on Windows.
struct Shutdown {
    #[cfg(unix)]
    signals: [tokio::signal::unix::Signal; 3],
    #[cfg(windows)]
    ctrl_c: tokio::signal::windows::CtrlC,
    #[cfg(windows)]
    ctrl_break: tokio::signal::windows::CtrlBreak,
    #[cfg(windows)]
    ctrl_close: tokio::signal::windows::CtrlClose,
}

impl Shutdown {
    #[cfg(unix)]
    fn new() -> io::Result<Self> {
        use tokio::signal::unix::{SignalKind, signal};

        Ok(Self {
            signals: [
                signal(SignalKind::interrupt())?,
                signal(SignalKind::terminate())?,
                signal(SignalKind::hangup())?,
            ],
        })
    }

    #[cfg(windows)]
    fn new() -> io::Result<Self> {
        use tokio::signal::windows;

        Ok(Self {
            ctrl_c: windows::ctrl_c()?,
            ctrl_break: windows::ctrl_break()?,
            ctrl_close: windows::ctrl_close()?,
        })
    }

    /// Returns once a signal asks to stop.
    #[cfg(unix)]
    async fn requested(&mut self) {
        let [interrupt, terminate, hangup] = &mut self.signals;

        tokio::select! {
            _ = interrupt.recv() => {}
            _ = terminate.recv() => {}
            _ = hangup.recv() => {}
        }
    }

    /// Returns once a signal asks to stop.
    #[cfg(windows)]
    async fn requested(&mut self) {
        tokio::select! {
            _ = self.ctrl_c.recv() => {}
            _ = self.ctrl_break.recv() => {}
            _ = self.ctrl_close.recv() => {}
        }
    }
}
