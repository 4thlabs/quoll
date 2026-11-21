//! The workspace under `tests/fixture`, copied for each test, with its binaries built: a contract whose binary prints
//! its document, `record`, a command that writes down each of its runs, a server of the contract, and `web-dev`, a
//! `web.dev` that listens on a port.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use tempfile::TempDir;

/// A copy of the workspace under `tests/fixture`, with its binaries built.
pub struct Fixture {
    dir: TempDir,
}

impl Fixture {
    /// The workspace, with `config` written after its `[workspace]` table.
    pub fn new(config: &str) -> Self {
        let dir = tempfile::tempdir().expect("a temporary directory");

        copy_dir(
            &Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixture"),
            dir.path(),
        );
        fs::write(
            dir.path().join("Cargo.toml"),
            format!(
                "[workspace]\nmembers = [\"contract\", \"server\", \"web-dev\"]\nresolver = \"3\"\n{config}"
            ),
        )
        .expect("the workspace's Cargo.toml is written");

        // `record` and `web-dev` are found on the PATH, as a project's own commands are: they are built first.
        let status = cargo()
            .args(["build", "--quiet"])
            .current_dir(dir.path())
            .env_remove("CARGO_TARGET_DIR")
            .env_remove("CARGO_BUILD_TARGET_DIR")
            .status()
            .expect("cargo runs");
        assert!(status.success(), "the fixture builds");

        Self { dir }
    }

    pub fn path(&self, relative: &str) -> PathBuf {
        self.dir.path().join(relative)
    }

    /// `cargo quoll`, to run in the workspace's root, with its binaries on the PATH.
    pub fn command(&self) -> Command {
        let path = env::var_os("PATH").unwrap_or_default();
        let path = env::join_paths(
            std::iter::once(self.path("target/debug")).chain(env::split_paths(&path)),
        )
        .expect("the PATH joins");
        let mut command = Command::new(env!("CARGO_BIN_EXE_cargo-quoll"));

        command
            .arg("quoll")
            .current_dir(self.dir.path())
            .env("PATH", path)
            .env_remove("CARGO_TARGET_DIR")
            .env_remove("CARGO_BUILD_TARGET_DIR");

        command
    }
}

fn copy_dir(from: &Path, to: &Path) {
    fs::create_dir_all(to).expect("the directory is made");

    for entry in fs::read_dir(from).expect("the fixture is there") {
        let entry = entry.expect("the fixture is read");
        let target = to.join(entry.file_name());

        // What a build of the fixture where it lies leaves there.
        if entry.file_name() == "target" || entry.file_name() == "Cargo.lock" {
            continue;
        }

        if entry.file_type().expect("the fixture is read").is_dir() {
            copy_dir(&entry.path(), &target);
        } else {
            fs::copy(entry.path(), target).expect("the fixture is copied");
        }
    }
}

fn cargo() -> Command {
    Command::new(env::var_os("CARGO").unwrap_or_else(|| "cargo".into()))
}
