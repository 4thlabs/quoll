//! The OpenAPI documents of the workspace's contracts: printed by their binaries, written, and checked.

use std::fs;
use std::io;
use std::path::Path;
use std::process::{Command, Stdio};

use anyhow::{Context, Result, bail};

use crate::cargo::Workspace;
use crate::config::Api;

/// Writes each api's document when its content changed, then runs its `after` commands in order, whether it
/// changed or not.
pub(crate) fn generate(workspace: &Workspace) -> Result<()> {
    for api in &workspace.config.apis {
        let document = print(workspace, api)?;
        let path = workspace.root.join(&api.output);

        if write(&path, &document).with_context(|| format!("{}: cannot write it", output(api)))? {
            eprintln!("{:>12} {}", "Wrote", api.output);
        } else {
            eprintln!("{:>12} {}", "Unchanged", api.output);
        }

        let dir = path.parent().unwrap_or(&workspace.root);
        for command in &api.after {
            command.run(dir)?;
        }
    }

    Ok(())
}

/// Writes nothing, and fails when a document on disk is not the one its contract prints, or is missing.
pub(crate) fn check(workspace: &Workspace) -> Result<()> {
    let mut outdated = Vec::new();

    for api in &workspace.config.apis {
        let document = print(workspace, api)?;
        let path = workspace.root.join(&api.output);

        match fs::read(&path) {
            Ok(written) if written == document => {}
            Ok(_) => outdated.push(format!("{}: not the one `{}` prints", api.output, api.bin)),
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                outdated.push(format!("{}: missing", api.output));
            }
            Err(error) => {
                return Err(error).with_context(|| format!("{}: cannot read it", output(api)));
            }
        }
    }

    if !outdated.is_empty() {
        bail!(
            "documents not up to date, which `cargo quoll gen` writes:\n  {}",
            outdated.join("\n  ")
        );
    }

    eprintln!(
        "{:>12} {} document(s) up to date",
        "Checked",
        workspace.config.apis.len()
    );

    Ok(())
}

/// The document `api`'s binary prints, built and run in the workspace's root.
fn print(workspace: &Workspace, api: &Api) -> Result<Vec<u8>> {
    let package = workspace.package(&api.package);
    let executable = workspace
        .build_binary(package, &api.bin)
        .with_context(|| api.key.clone())?;

    let output = Command::new(&executable)
        .current_dir(&workspace.root)
        .stdin(Stdio::null())
        .stderr(Stdio::inherit())
        .output()
        .with_context(|| format!("{}: cannot run {}", api.key, executable.display()))?;

    if !output.status.success() {
        bail!(
            "{}: the binary `{}` of `{}` failed, {}",
            api.key,
            api.bin,
            api.package,
            output.status
        );
    }

    Ok(output.stdout)
}

/// Writes `document` at `path` unless it is there already, and says whether it wrote it.
fn write(path: &Path, document: &[u8]) -> io::Result<bool> {
    match fs::read(path) {
        Ok(written) if written == document => return Ok(false),
        Ok(_) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }

    if let Some(dir) = path.parent() {
        fs::create_dir_all(dir)?;
    }
    fs::write(path, document)?;

    Ok(true)
}

/// The key of `api`'s output, which its errors name.
fn output(api: &Api) -> String {
    format!("{}.output", api.key)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_document_is_written_where_its_directory_is_missing() {
        let dir = tempfile::tempdir().expect("a temporary directory");
        let path = dir.path().join("web/openapi.json");

        assert!(write(&path, b"{}").expect("the document is written"));
        assert_eq!(fs::read(&path).expect("the document is there"), b"{}");
    }

    #[test]
    fn a_document_that_did_not_change_is_not_written() {
        let dir = tempfile::tempdir().expect("a temporary directory");
        let path = dir.path().join("openapi.json");

        assert!(write(&path, b"{}").expect("the document is written"));
        assert!(!write(&path, b"{}").expect("the document is read"));
        assert!(write(&path, b"[]").expect("the document is written again"));
        assert_eq!(fs::read(&path).expect("the document is there"), b"[]");
    }
}
