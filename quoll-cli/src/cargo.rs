//! What cargo says of the workspace, and the binaries it builds.

use std::env;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use anyhow::{Context, Result, bail};
use serde::Deserialize;
use serde_json::Value;

use crate::config::Config;

/// The workspace `cargo quoll` runs in, found from the current directory as cargo finds it.
#[derive(Debug)]
pub(crate) struct Workspace {
    pub(crate) root: PathBuf,
    /// Where cargo builds, `target/` unless the workspace or the environment says otherwise.
    pub(crate) target_directory: PathBuf,
    pub(crate) packages: Vec<Package>,
    pub(crate) config: Config,
}

/// A package of the workspace, as `cargo metadata` describes it.
#[derive(Debug, Deserialize)]
pub(crate) struct Package {
    pub(crate) name: String,
    pub(crate) id: String,
    manifest_path: PathBuf,
    targets: Vec<Target>,
    #[serde(default)]
    dependencies: Vec<Dependency>,
    /// The binary `cargo run` runs when the package has several, `default-run` in its `Cargo.toml`.
    default_run: Option<String>,
}

#[derive(Debug, Deserialize)]
struct Target {
    name: String,
    kind: Vec<String>,
}

#[derive(Debug, Deserialize)]
struct Dependency {
    /// `dev` or `build`, null for a normal dependency.
    kind: Option<String>,
    /// The directory of a path dependency.
    path: Option<PathBuf>,
}

impl Package {
    /// Whether the package has a binary named `name`.
    pub(crate) fn has_binary(&self, name: &str) -> bool {
        self.binaries().any(|binary| binary == name)
    }

    /// The names of the package's binaries.
    pub(crate) fn binaries(&self) -> impl Iterator<Item = &str> {
        self.targets
            .iter()
            .filter(|target| target.kind.iter().any(|kind| kind == "bin"))
            .map(|target| target.name.as_str())
    }

    /// The binary `cargo run` runs: `default-run`, or the package's only binary.
    pub(crate) fn default_binary(&self) -> Option<&str> {
        if let Some(name) = &self.default_run {
            return Some(name);
        }

        let mut binaries = self.binaries();

        match (binaries.next(), binaries.next()) {
            (Some(binary), None) => Some(binary),
            _ => None,
        }
    }

    /// The directory of the package, where its `Cargo.toml` is.
    pub(crate) fn dir(&self) -> &Path {
        self.manifest_path
            .parent()
            .expect("a manifest is a file, in a directory")
    }

    /// The directories of the path dependencies that its binaries are built with, its dev-dependencies left out.
    pub(crate) fn path_dependencies(&self) -> impl Iterator<Item = &Path> {
        self.dependencies
            .iter()
            .filter(|dependency| dependency.kind.as_deref() != Some("dev"))
            .filter_map(|dependency| dependency.path.as_deref())
    }
}

#[derive(Deserialize)]
struct Metadata {
    workspace_root: PathBuf,
    target_directory: PathBuf,
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
            target_directory: metadata.target_directory,
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
        let output = self
            .build_command(package, bin, "json-render-diagnostics", lock)
            .stderr(Stdio::inherit())
            .output()
            .context("cannot run `cargo build`")?;

        if !output.status.success() {
            bail!("cargo cannot build the binary `{bin}` of `{name}`");
        }

        executable(&output.stdout, package, bin)?.with_context(|| {
            format!("cargo built no executable for the binary `{bin}` of `{name}`")
        })
    }

    /// `cargo build` of the binary `bin` of `package`, in the workspace's root, its messages in JSON on its stdout,
    /// in the `--message-format` named `format`.
    pub(crate) fn build_command(
        &self,
        package: &Package,
        bin: &str,
        format: &str,
        lock: Lock,
    ) -> Command {
        let mut build = cargo();

        build
            .args(["build", "--package", &package.name, "--bin", bin])
            .arg(format!("--message-format={format}"))
            .current_dir(&self.root);

        if lock == Lock::Locked {
            build.arg("--locked");
        }

        build
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

/// A message of `cargo build --message-format=json`, one of its JSON lines, with what quoll reads of it.
#[derive(Debug)]
pub(crate) enum Message {
    /// A target built, or found fresh.
    Artifact(Artifact),
    /// A message of the compiler: an error, a warning.
    Diagnostic(CompilerMessage),
    /// Any other message.
    Other,
}

#[derive(Debug, Deserialize)]
pub(crate) struct Artifact {
    package_id: String,
    target: Target,
    pub(crate) executable: Option<PathBuf>,
    /// Whether cargo found the target up to date, and built nothing.
    #[serde(default)]
    pub(crate) fresh: bool,
}

impl Artifact {
    /// Whether the artifact is the binary `bin` of `package`.
    pub(crate) fn is_binary(&self, package: &Package, bin: &str) -> bool {
        self.package_id == package.id
            && self.target.name == bin
            && self.target.kind.iter().any(|kind| kind == "bin")
    }
}

#[derive(Debug, Deserialize)]
pub(crate) struct CompilerMessage {
    pub(crate) package_id: String,
    pub(crate) target: CompilerTarget,
    pub(crate) message: Diagnostic,
}

#[derive(Debug, Deserialize)]
pub(crate) struct CompilerTarget {
    pub(crate) name: String,
}

/// A diagnostic of rustc, as its JSON output describes it.
#[derive(Debug, Deserialize)]
pub(crate) struct Diagnostic {
    pub(crate) message: String,
    /// `error`, `warning`, `note`...
    pub(crate) level: String,
    /// The diagnostic as rustc prints it, when it prints it.
    pub(crate) rendered: Option<String>,
    #[serde(default)]
    pub(crate) spans: Vec<Span>,
}

#[derive(Debug, Deserialize)]
pub(crate) struct Span {
    /// Relative to the workspace's root, where cargo runs rustc, for a package of the workspace.
    pub(crate) file_name: String,
    pub(crate) line_start: u64,
    pub(crate) column_start: u64,
    pub(crate) is_primary: bool,
}

impl Diagnostic {
    /// The span the diagnostic points at first.
    pub(crate) fn primary_span(&self) -> Option<&Span> {
        self.spans.iter().find(|span| span.is_primary)
    }
}

impl Message {
    /// Reads a line of cargo's output: an error when it is not one of its JSON messages.
    pub(crate) fn parse(line: &[u8]) -> Result<Self> {
        #[derive(Deserialize)]
        struct Reason {
            reason: String,
        }

        let read = || {
            format!(
                "cannot read a message of cargo: {}",
                String::from_utf8_lossy(line)
            )
        };
        let reason: Reason = serde_json::from_slice(line).with_context(read)?;

        match reason.reason.as_str() {
            "compiler-artifact" => Ok(Self::Artifact(
                serde_json::from_slice(line).with_context(read)?,
            )),
            "compiler-message" => Ok(Self::Diagnostic(
                serde_json::from_slice(line).with_context(read)?,
            )),
            _ => Ok(Self::Other),
        }
    }
}

/// The executable of `bin` in the messages of `cargo build --message-format=json`, which are JSON lines.
fn executable(messages: &[u8], package: &Package, bin: &str) -> Result<Option<PathBuf>> {
    for line in messages.split(|byte| *byte == b'\n') {
        if line.iter().all(u8::is_ascii_whitespace) {
            continue;
        }

        if let Message::Artifact(artifact) = Message::parse(line)?
            && artifact.is_binary(package, bin)
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
            "manifest_path": "/notes/api/Cargo.toml",
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
