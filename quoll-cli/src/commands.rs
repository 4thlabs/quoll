//! The commands a config names, run with no shell.

use std::env;
use std::fmt;
use std::path::Path;
use std::process::{self, ExitStatus};

use anyhow::{Context, Result, bail};

/// A command of the config: its program, then its arguments, and the key it is written under.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Command {
    /// Never empty, as the config refuses an empty command.
    words: Vec<String>,
    key: String,
}

impl Command {
    /// A command of `words`, which hold its program at least, written under `key`.
    pub(crate) fn new(words: Vec<String>, key: String) -> Self {
        Self { words, key }
    }

    /// Runs the command in `dir`, its output passed through, and fails when it does.
    ///
    /// Its program is found as [`Command::process`] says.
    pub(crate) fn run(&self, dir: &Path) -> Result<()> {
        let status = self
            .process(dir)?
            .status()
            .with_context(|| format!("{}: cannot start `{self}`", self.key))?;

        self.check(status)
    }

    /// The process of the command, to run in `dir`, which says it runs.
    ///
    /// Its program is found on the `PATH` as the OS does, with `PATHEXT` on Windows, where `npm` is `npm.cmd`, or
    /// from `dir` when it is written as a path.
    pub(crate) fn process(&self, dir: &Path) -> Result<process::Command> {
        let key = &self.key;

        if !dir.is_dir() {
            bail!(
                "{key}: cannot run `{self}` in {}: no such directory",
                dir.display()
            );
        }

        let program = &self.words[0];
        let resolved = which::which_in(program, env::var_os("PATH"), dir)
            .with_context(|| format!("{key}: cannot find the program `{program}` on the PATH"))?;

        eprintln!("{:>12} `{self}` in {}", "Running", dir.display());

        let mut process = process::Command::new(&resolved);

        process.args(&self.words[1..]).current_dir(dir);

        Ok(process)
    }

    /// Fails when the command exited with `status`, a failure.
    pub(crate) fn check(&self, status: ExitStatus) -> Result<()> {
        if !status.success() {
            bail!("{}: `{self}` failed, {status}", self.key);
        }

        Ok(())
    }

    /// The key the command is written under.
    pub(crate) fn key(&self) -> &str {
        &self.key
    }
}

impl fmt::Display for Command {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.words.join(" "))
    }
}
