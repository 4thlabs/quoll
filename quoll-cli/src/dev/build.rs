//! The builds of `cargo quoll dev`: cargo's diagnostics read from its JSON messages, printed and reported as they
//! come, and the executable it built.

use std::io::{self, IsTerminal};
use std::path::PathBuf;
use std::process::Stdio;

use anyhow::{Context, Result, bail};
use tokio::io::{AsyncBufReadExt, BufReader};

use crate::cargo::{Lock, Message, Package, Workspace};
use crate::dev::events::Reporter;
use crate::dev::process::Child;

/// A binary cargo built.
#[derive(Debug)]
pub(crate) struct Built {
    pub(crate) executable: PathBuf,
    /// Where its dynamic libraries are, as `cargo run` gives them: the native libraries of build scripts in the
    /// target directory, then the dependencies and the executable's own directory.
    pub(crate) library_paths: Vec<PathBuf>,
}

/// Builds the binary `bin` of `package`. Each diagnostic of rustc is printed, and each error is an event.
pub(crate) async fn build(
    workspace: &Workspace,
    package: &Package,
    bin: &str,
    reporter: &Reporter,
) -> Result<Built> {
    let name = &package.name;
    // The diagnostics keep their colours where cargo's own output would have them.
    let format = if io::stderr().is_terminal() {
        "json-diagnostic-rendered-ansi"
    } else {
        "json"
    };
    let mut command = workspace.build_command(package, bin, format, Lock::Update);

    command.stdout(Stdio::piped()).stderr(Stdio::inherit());

    let mut cargo = Child::spawn(command).context("cannot run `cargo build`")?;
    let stdout = cargo.stdout().expect("cargo's stdout is piped");
    let mut lines = BufReader::new(stdout).lines();
    let mut built = None;
    let mut native = Vec::new();

    while let Some(line) = lines
        .next_line()
        .await
        .context("cannot read the output of `cargo build`")?
    {
        if line.trim().is_empty() {
            continue;
        }

        match Message::parse(line.as_bytes())? {
            Message::Artifact(artifact) if artifact.is_binary(package, bin) => {
                built = artifact.executable;
            }
            Message::BuildScript(script) => native.extend(
                script
                    .linked_paths
                    .iter()
                    .map(|path| {
                        PathBuf::from(path.split_once('=').map_or(path.as_str(), |(_, path)| path))
                    })
                    // Cargo gives those of the target directory alone, as the others are the system's.
                    .filter(|path| path.starts_with(&workspace.target_directory)),
            ),
            Message::Diagnostic(message) => {
                let package = workspace
                    .packages
                    .iter()
                    .find(|package| package.has_manifest(&message.manifest_path))
                    .map_or(message.target.name.as_str(), |package| &package.name);

                reporter.diagnostic(package, &message.message);
            }
            _ => {}
        }
    }

    let status = cargo
        .wait()
        .await
        .context("cannot wait for `cargo build`")?;

    if !status.success() {
        bail!("cargo cannot build the binary `{bin}` of `{name}`");
    }

    let executable = built
        .with_context(|| format!("cargo built no executable for the binary `{bin}` of `{name}`"))?;
    let dir = executable
        .parent()
        .context("an executable is in a directory")?
        .to_path_buf();

    native.push(dir.join("deps"));
    native.push(dir);

    Ok(Built {
        executable,
        library_paths: native,
    })
}

/// The variable of the paths the dynamic loader searches, as cargo sets it for `cargo run`.
pub(crate) const LIBRARY_PATH: &str = if cfg!(windows) {
    "PATH"
} else if cfg!(target_os = "macos") {
    "DYLD_FALLBACK_LIBRARY_PATH"
} else {
    "LD_LIBRARY_PATH"
};
