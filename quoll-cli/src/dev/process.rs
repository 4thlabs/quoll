//! The children of `cargo quoll dev`, each in a process group of its own on Unix and in a job object on Windows, so
//! that stopping one stops what it started: `npm run dev` and the bundler under it, cargo and its rustc.

use std::io;
use std::process::{self, ExitStatus, Stdio};
use std::sync::Mutex;
use std::time::Duration;

use process_wrap::tokio::{ChildWrapper, CommandWrap, KillOnDrop};
use tokio::process::ChildStdout;
use tokio::task::JoinHandle;

/// How long a child and what it started have to stop once asked, before they are killed.
const GRACE: Duration = Duration::from_secs(5);

/// How often a group whose leader exited is looked at again, until its last process is gone.
#[cfg(unix)]
const POLL: Duration = Duration::from_millis(50);

/// The stops of the children that were dropped while they ran, a round cut short by a signal: `cargo quoll dev`
/// waits for them before it exits.
static STOPPING: Mutex<Vec<JoinHandle<()>>> = Mutex::new(Vec::new());

/// A child, stopped with all it started when it is dropped: asked to stop when it still runs, killed with what is left
/// of its group when it exited.
#[derive(Debug)]
pub(crate) struct Child {
    /// Always there, but in `drop`.
    group: Option<Group>,
}

/// A child's process with its group on Unix, its job on Windows, whatever is left of them killed when it is dropped.
#[derive(Debug)]
struct Group {
    inner: Box<dyn ChildWrapper>,
    /// The id of the group, the leader's pid.
    #[cfg(unix)]
    id: nix::unistd::Pid,
}

impl Child {
    /// Starts `command`, its stdin closed: in a group of its own, a child that reads the terminal would be stopped
    /// by it, as a background job is.
    pub(crate) fn spawn(command: process::Command) -> io::Result<Self> {
        let mut command = tokio::process::Command::from(command);

        command.stdin(Stdio::null());

        let mut wrap = CommandWrap::from(command);

        // A group of its own, which a Ctrl-C in the terminal does not reach: `cargo quoll dev` stops it, in its order.
        #[cfg(unix)]
        wrap.wrap(process_wrap::tokio::ProcessGroup::leader());

        #[cfg(windows)]
        {
            use process_wrap::tokio::{CreationFlags, JobObject};
            use windows::Win32::System::Threading::CREATE_NEW_PROCESS_GROUP;

            wrap.wrap(CreationFlags(CREATE_NEW_PROCESS_GROUP));
            wrap.wrap(JobObject);
        }

        // On Windows, the job is killed when its handle closes, so that the children go when `cargo quoll dev` is
        // killed itself.
        wrap.wrap(KillOnDrop);

        let inner = wrap.spawn()?;

        #[cfg(unix)]
        let id = {
            let pid = inner
                .id()
                .and_then(|pid| i32::try_from(pid).ok())
                .expect("a child that just started has its pid");

            nix::unistd::Pid::from_raw(pid)
        };

        Ok(Self {
            group: Some(Group {
                inner,
                #[cfg(unix)]
                id,
            }),
        })
    }

    fn group(&mut self) -> &mut Group {
        self.group
            .as_mut()
            .expect("a child has its group until it is dropped")
    }

    /// The child's stdout, once, when it is piped.
    pub(crate) fn stdout(&mut self) -> Option<ChildStdout> {
        self.group().inner.stdout().take()
    }

    /// Waits for the child to exit. On Unix, what it started may still run, which `stop` or the drop end.
    pub(crate) async fn wait(&mut self) -> io::Result<ExitStatus> {
        self.group().inner.wait().await
    }

    /// The child's status when it exited, none when it runs.
    pub(crate) fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        self.group().inner.try_wait()
    }

    /// Stops the child and what it started, and returns the child's status. On Unix, its group is asked to stop with
    /// `SIGTERM`, then killed once its last process is gone or after a grace. On Windows, its job is terminated at
    /// once: a console's Ctrl-Break, the gentle stop, is sent through an API this crate, which has no `unsafe`, does
    /// not call.
    pub(crate) async fn stop(mut self) -> io::Result<ExitStatus> {
        self.group
            .take()
            .expect("a child has its group until it is dropped")
            .stop()
            .await
    }
}

impl Drop for Child {
    fn drop(&mut self) {
        let Some(mut group) = self.group.take() else {
            return;
        };

        // A child that still runs is the one of a round a signal cut short: it is asked to stop, as at the end, and
        // `cargo quoll dev` waits for it. With no runtime left, it is killed with its group.
        if let Ok(None) = group.inner.try_wait()
            && let Ok(runtime) = tokio::runtime::Handle::try_current()
        {
            let stopping = runtime.spawn(async move {
                // Its status is no one's: the round that would have read it is gone.
                let _ = group.stop().await;
            });

            STOPPING
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .push(stopping);
        }
    }
}

impl Group {
    async fn stop(mut self) -> io::Result<ExitStatus> {
        #[cfg(unix)]
        if nix::sys::signal::killpg(self.id, nix::sys::signal::Signal::SIGTERM).is_ok() {
            let deadline = tokio::time::Instant::now() + GRACE;

            if let Ok(status) = tokio::time::timeout_at(deadline, self.inner.wait()).await {
                // The leader's wait reaps its own: what it started is another's child, and is waited for apart.
                let _ = tokio::time::timeout_at(deadline, self.gone()).await;

                return status;
            }
        }

        #[cfg(windows)]
        let _ = GRACE;

        // A child that exited already cannot be killed: its status is the one waited for.
        let _ = self.inner.start_kill();

        self.inner.wait().await
    }

    /// Returns once no process is left in the group.
    #[cfg(unix)]
    async fn gone(&self) {
        while nix::sys::signal::killpg(self.id, None).is_ok() {
            tokio::time::sleep(POLL).await;
        }
    }
}

impl Drop for Group {
    fn drop(&mut self) {
        // What is left of the group or of the job, the child itself when it still runs. A child that exited with all
        // it started cannot be killed, which is no failure.
        let _ = self.inner.start_kill();
    }
}

/// Waits for the stops of the children dropped while they ran.
pub(crate) async fn stopped() {
    loop {
        let stopping = std::mem::take(
            &mut *STOPPING
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner),
        );

        if stopping.is_empty() {
            return;
        }

        for stop in stopping {
            // A stop that panicked left its group to the drop, which killed it.
            let _ = stop.await;
        }
    }
}

/// The exit code of a status, none for a process that a signal ended.
pub(crate) fn code(status: &io::Result<ExitStatus>) -> Option<i32> {
    status.as_ref().ok().and_then(ExitStatus::code)
}
