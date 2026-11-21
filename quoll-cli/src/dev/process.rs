//! The children of `cargo quoll dev`, each in a process group of its own on Unix and in a job object on Windows, so
//! that stopping one stops what it started: `npm run dev` and the bundler under it, cargo and its rustc.

use std::io;
use std::process::{self, ExitStatus, Stdio};
use std::time::Duration;

use process_wrap::tokio::{ChildWrapper, CommandWrap, KillOnDrop};
use tokio::process::ChildStdout;

/// How long a child has to stop once asked, before it is killed.
const GRACE: Duration = Duration::from_secs(5);

/// A child, killed with all it started when it is dropped.
#[derive(Debug)]
pub(crate) struct Child {
    inner: Box<dyn ChildWrapper>,
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

        Ok(Self {
            inner: wrap.spawn()?,
        })
    }

    /// The child's stdout, once, when it is piped.
    pub(crate) fn stdout(&mut self) -> Option<ChildStdout> {
        self.inner.stdout().take()
    }

    /// Waits for the child to exit, and for the rest of its group on Unix, of its job on Windows.
    pub(crate) async fn wait(&mut self) -> io::Result<ExitStatus> {
        self.inner.wait().await
    }

    /// The child's status when it exited, none when it runs.
    pub(crate) fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        self.inner.try_wait()
    }

    /// Stops the child and what it started. On Unix, its group is asked to stop with `SIGTERM`, then killed after a
    /// grace. On Windows, its job is terminated at once: a console's Ctrl-Break, the gentle stop, is sent through an
    /// API this crate, which has no `unsafe`, does not call.
    pub(crate) async fn stop(mut self) -> io::Result<ExitStatus> {
        #[cfg(unix)]
        if self.inner.signal(libc::SIGTERM).is_ok()
            && let Ok(status) = tokio::time::timeout(GRACE, self.inner.wait()).await
        {
            return status;
        }

        #[cfg(windows)]
        let _ = GRACE;

        // A child that exited already cannot be killed: its status is the one waited for.
        let _ = self.inner.start_kill();

        self.inner.wait().await
    }
}

impl Drop for Child {
    fn drop(&mut self) {
        // What is left of the group or of the job, the child itself when it still runs. A child that exited with all
        // it started cannot be killed, which is no failure.
        let _ = self.inner.start_kill();
    }
}

/// The exit code of a status, none for a process that a signal ended.
pub(crate) fn code(status: &io::Result<ExitStatus>) -> Option<i32> {
    status.as_ref().ok().and_then(ExitStatus::code)
}
