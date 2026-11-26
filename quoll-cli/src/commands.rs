//! The commands a config names, run with no shell.

use std::env;
use std::fmt;
use std::io::{self, Write};
use std::path::Path;
use std::process::{self, ExitStatus};

use anyhow::{Context, Result, bail};

/// A command of the config: its program, then its arguments, the key it is written under, and the variables it runs
/// with.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Command {
    /// Never empty, as the config refuses an empty command.
    words: Vec<String>,
    key: String,
    /// Set on top of the environment of `cargo quoll`.
    env: Vec<(String, String)>,
}

impl Command {
    /// A command of `words`, which hold its program at least, written under `key`.
    pub(crate) fn new(words: Vec<String>, key: String) -> Self {
        Self {
            words,
            key,
            env: Vec::new(),
        }
    }

    /// The command, run with `env` set on top of the environment of `cargo quoll`.
    pub(crate) fn with_env(self, env: Vec<(String, String)>) -> Self {
        Self { env, ..self }
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
    /// from `dir` when it is written as a path. A `PATH` of its `env` is the one searched, as the OS would.
    pub(crate) fn process(&self, dir: &Path) -> Result<process::Command> {
        let key = &self.key;

        if !dir.is_dir() {
            bail!(
                "{key}: cannot run `{self}` in {}: no such directory",
                dir.display()
            );
        }

        let program = &self.words[0];
        let path = self
            .env
            .iter()
            .find(|(name, _)| is_variable(name, "PATH"))
            .map(|(_, value)| value.into())
            .or_else(|| env::var_os("PATH"));
        let resolved = which::which_in(program, path, dir)
            .with_context(|| format!("{key}: cannot find the program `{program}` on the PATH"))?;

        // Not `eprintln!`, which panics on a terminal hung up, and would kill the children of `cargo quoll dev`.
        let _ = writeln!(
            io::stderr(),
            "{:>12} `{self}` in {}",
            "Running",
            dir.display()
        );

        let mut process = process::Command::new(&resolved);

        process
            .args(&self.words[1..])
            .envs(self.env.iter().map(|(name, value)| (name, value)))
            .current_dir(dir);

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

/// Whether `name` names the variable `variable`, in any case on Windows, as the OS reads it there.
pub(crate) fn is_variable(name: &str, variable: &str) -> bool {
    if cfg!(windows) {
        name.eq_ignore_ascii_case(variable)
    } else {
        name == variable
    }
}

impl fmt::Display for Command {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.words.join(" "))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_path_of_the_env_is_the_one_searched() {
        let executable = env::current_exe().expect("the test knows its executable");
        let dir = executable
            .parent()
            .expect("an executable is in a directory");
        let name = executable
            .file_stem()
            .and_then(|name| name.to_str())
            .expect("the name of the test is UTF-8");
        let command = Command::new(vec![name.to_owned()], "web.dev".to_owned()).with_env(vec![(
            "PATH".to_owned(),
            dir.to_str().expect("the path is UTF-8").to_owned(),
        )]);

        let process = command.process(dir).expect("the program is found");

        assert_eq!(process.get_program(), executable);
        assert!(
            process
                .get_envs()
                .any(|(name, value)| name == "PATH" && value == Some(dir.as_os_str())),
            "the child gets the PATH too"
        );
    }
}
