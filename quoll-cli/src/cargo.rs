//! What cargo says of the workspace, and the binaries it builds.

use std::env;
use std::ffi::OsString;
use std::path::PathBuf;
use std::process::{Command, Stdio};

use anyhow::{Context, Result, bail};
use serde::Deserialize;
use serde_json::Value;

use crate::config::Config;

/// The workspace `cargo quoll` runs in, found from the current directory as cargo finds it.
#[derive(Debug)]
pub(crate) struct Workspace {
    pub(crate) root: PathBuf,
    pub(crate) packages: Vec<Package>,
    pub(crate) config: Config,
}

/// A package of the workspace, as `cargo metadata` describes it.
#[derive(Debug, Deserialize)]
pub(crate) struct Package {
    pub(crate) name: String,
    id: String,
    targets: Vec<Target>,
}

#[derive(Debug, Deserialize)]
struct Target {
    name: String,
    kind: Vec<String>,
}

impl Package {
    /// Whether the package has a binary named `name`.
    pub(crate) fn has_binary(&self, name: &str) -> bool {
        self.targets
            .iter()
            .any(|target| target.name == name && target.kind.iter().any(|kind| kind == "bin"))
    }
}

#[derive(Deserialize)]
struct Metadata {
    workspace_root: PathBuf,
    packages: Vec<Package>,
    /// `[workspace.metadata]`, null when there is none.
    metadata: Value,
}

impl Workspace {
    /// Reads the workspace and its config through `cargo metadata`.
    pub(crate) fn load() -> Result<Self> {
        let output = cargo()
            .args(["metadata", "--format-version", "1", "--no-deps"])
            .stderr(Stdio::inherit())
            .output()
            .context("cannot run `cargo metadata`")?;

        if !output.status.success() {
            bail!("`cargo metadata` failed, {}", output.status);
        }

        let metadata: Metadata = serde_json::from_slice(&output.stdout)
            .context("cannot read the output of `cargo metadata`")?;
        let config = Config::read(&metadata.metadata, &metadata.packages).with_context(|| {
            format!(
                "cannot read the config in {}",
                metadata.workspace_root.join("Cargo.toml").display()
            )
        })?;

        Ok(Self {
            root: metadata.workspace_root,
            packages: metadata.packages,
            config,
        })
    }

    /// The package of the workspace named `name`, which the config has checked is there.
    pub(crate) fn package(&self, name: &str) -> &Package {
        self.packages
            .iter()
            .find(|package| package.name == name)
            .expect("the config names packages of the workspace only")
    }

    /// Builds the binary `bin` of `package` and returns the path of its executable. Cargo's own output, its errors
    /// included, is passed through.
    pub(crate) fn build_binary(&self, package: &Package, bin: &str) -> Result<PathBuf> {
        let name = &package.name;
        let output = cargo()
            .args(["build", "--package", name, "--bin", bin])
            .arg("--message-format=json-render-diagnostics")
            .current_dir(&self.root)
            .stderr(Stdio::inherit())
            .output()
            .context("cannot run `cargo build`")?;

        if !output.status.success() {
            bail!("cargo cannot build the binary `{bin}` of `{name}`");
        }

        executable(&output.stdout, package, bin).with_context(|| {
            format!("cargo built no executable for the binary `{bin}` of `{name}`")
        })
    }
}

/// The executable of `bin` in the messages of `cargo build --message-format=json`.
fn executable(messages: &[u8], package: &Package, bin: &str) -> Option<PathBuf> {
    #[derive(Deserialize)]
    struct Message {
        reason: String,
        package_id: Option<String>,
        target: Option<Target>,
        executable: Option<PathBuf>,
    }

    messages
        .split(|byte| *byte == b'\n')
        .filter_map(|line| serde_json::from_slice::<Message>(line).ok())
        .filter(|message| {
            message.reason == "compiler-artifact"
                && message.package_id.as_deref() == Some(package.id.as_str())
                && message.target.as_ref().is_some_and(|target| {
                    target.name == bin && target.kind.iter().any(|kind| kind == "bin")
                })
        })
        .find_map(|message| message.executable)
}

/// Cargo, the one that runs `cargo quoll` when it sets `CARGO`, as it does for its subcommands.
fn cargo() -> Command {
    Command::new(env::var_os("CARGO").unwrap_or_else(|| OsString::from("cargo")))
}
