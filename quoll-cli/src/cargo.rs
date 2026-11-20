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
    pub(crate) fn build_binary(&self, package: &Package, bin: &str, lock: Lock) -> Result<PathBuf> {
        let name = &package.name;
        let mut build = cargo();

        build
            .args(["build", "--package", name, "--bin", bin])
            .arg("--message-format=json-render-diagnostics")
            .current_dir(&self.root)
            .stderr(Stdio::inherit());

        if lock == Lock::Locked {
            build.arg("--locked");
        }

        let output = build.output().context("cannot run `cargo build`")?;

        if !output.status.success() {
            bail!("cargo cannot build the binary `{bin}` of `{name}`");
        }

        executable(&output.stdout, package, bin)?.with_context(|| {
            format!("cargo built no executable for the binary `{bin}` of `{name}`")
        })
    }
}

/// Whether a build may update the workspace's `Cargo.lock`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Lock {
    /// As cargo does by default.
    Update,
    /// `--locked`: the build fails when the `Cargo.lock` would change.
    Locked,
}

/// The executable of `bin` in the messages of `cargo build --message-format=json`, which are JSON lines.
fn executable(messages: &[u8], package: &Package, bin: &str) -> Result<Option<PathBuf>> {
    #[derive(Deserialize)]
    struct Message {
        reason: String,
    }

    #[derive(Deserialize)]
    struct Artifact {
        package_id: String,
        target: Target,
        executable: Option<PathBuf>,
    }

    for line in messages.split(|byte| *byte == b'\n') {
        if line.iter().all(u8::is_ascii_whitespace) {
            continue;
        }

        let read = || {
            format!(
                "cannot read a message of cargo: {}",
                String::from_utf8_lossy(line)
            )
        };
        let message: Message = serde_json::from_slice(line).with_context(read)?;

        if message.reason != "compiler-artifact" {
            continue;
        }

        let artifact: Artifact = serde_json::from_slice(line).with_context(read)?;
        let target = &artifact.target;

        if artifact.package_id == package.id
            && target.name == bin
            && target.kind.iter().any(|kind| kind == "bin")
        {
            return Ok(artifact.executable);
        }
    }

    Ok(None)
}

/// Cargo, the one that runs `cargo quoll` when it sets `CARGO`, as it does for its subcommands.
fn cargo() -> Command {
    Command::new(env::var_os("CARGO").unwrap_or_else(|| OsString::from("cargo")))
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn package() -> Package {
        serde_json::from_value(json!({
            "name": "notes-api",
            "id": "path+file:///notes/api#notes-api@0.1.0",
            "targets": [{ "name": "openapi", "kind": ["bin"] }],
        }))
        .expect("the package is one of cargo metadata")
    }

    fn artifact(name: &str, kind: &str, executable: Option<&str>) -> String {
        json!({
            "reason": "compiler-artifact",
            "package_id": "path+file:///notes/api#notes-api@0.1.0",
            "target": { "name": name, "kind": [kind] },
            "executable": executable,
        })
        .to_string()
    }

    #[test]
    fn the_executable_of_the_binary_is_found() {
        let messages = [
            json!({ "reason": "build-script-executed", "package_id": "serde" }).to_string(),
            artifact("notes_api", "lib", None),
            artifact("openapi", "bin", Some("/target/debug/openapi")),
            json!({ "reason": "build-finished", "success": true }).to_string(),
        ]
        .join("\n");

        let executable =
            executable(messages.as_bytes(), &package(), "openapi").expect("the messages are read");

        assert_eq!(executable, Some(PathBuf::from("/target/debug/openapi")));
    }

    #[test]
    fn no_executable_is_none() {
        let messages = artifact("notes_api", "lib", None) + "\n";

        let executable =
            executable(messages.as_bytes(), &package(), "openapi").expect("the messages are read");

        assert_eq!(executable, None);
    }

    #[test]
    fn a_message_that_is_not_json_is_an_error() {
        let error = executable(b"Compiling notes-api\n", &package(), "openapi")
            .expect_err("the message is refused");

        assert_eq!(
            error.to_string(),
            "cannot read a message of cargo: Compiling notes-api"
        );
    }

    #[test]
    fn an_artifact_that_cannot_be_read_is_an_error() {
        let messages = json!({ "reason": "compiler-artifact", "target": "openapi" }).to_string();

        let error = executable(messages.as_bytes(), &package(), "openapi")
            .expect_err("the artifact is refused");

        assert!(
            error
                .to_string()
                .starts_with("cannot read a message of cargo: "),
            "{error}"
        );
    }
}
