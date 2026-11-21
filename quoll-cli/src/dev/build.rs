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
    /// Whether cargo found it up to date, and built nothing.
    pub(crate) fresh: bool,
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
                built = artifact.executable.map(|executable| Built {
                    executable,
                    fresh: artifact.fresh,
                });
            }
            Message::Diagnostic(message) => {
                let package = workspace
                    .packages
                    .iter()
                    .find(|package| package.id == message.package_id)
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

    built.with_context(|| format!("cargo built no executable for the binary `{bin}` of `{name}`"))
}
