//! The OpenAPI documents of the workspace's contracts: printed by their binaries, written, and checked.

use std::fs;
use std::io;
use std::path::Path;
use std::process::{self, Command, Stdio};

use anyhow::{Context, Result, bail};

use crate::cargo::{Lock, Workspace};
use crate::config::Api;

/// Writes each api's document when its content changed, then runs its `after` commands in order, whether it
/// changed or not.
pub(crate) fn generate(workspace: &Workspace) -> Result<()> {
    for api in &workspace.config.apis {
        let document = print(workspace, api, Lock::Update)?;
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

/// Writes nothing, and fails when a document on disk is not the one its contract prints, or is missing. Cargo builds
/// with `--locked`, so that it does not write the `Cargo.lock` either.
pub(crate) fn check(workspace: &Workspace) -> Result<()> {
    let mut outdated = Vec::new();

    for api in &workspace.config.apis {
        let document = print(workspace, api, Lock::Locked)?;
        let path = workspace.root.join(&api.output);

        match fs::read(&path) {
            Ok(written) if same(&written, &document) => {}
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
fn print(workspace: &Workspace, api: &Api, lock: Lock) -> Result<Vec<u8>> {
    let package = workspace.package(&api.package);
    let executable = workspace
        .build_binary(package, &api.bin, lock)
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
///
/// The new document is written beside it, then renamed over it, so that a watcher of `path` reads the old document or
/// the new one, never a part of it.
pub(crate) fn write(path: &Path, document: &[u8]) -> io::Result<bool> {
    match fs::read(path) {
        Ok(written) if same(&written, document) => return Ok(false),
        Ok(_) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }

    let (Some(dir), Some(name)) = (path.parent(), path.file_name()) else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "not the path of a file",
        ));
    };

    fs::create_dir_all(dir)?;

    let temporary = dir.join(format!(".{}.{}.tmp", name.to_string_lossy(), process::id()));
    let written = fs::write(&temporary, document).and_then(|()| fs::rename(&temporary, path));

    if written.is_err() {
        // The error of the write or of the rename is the one reported: a temporary file left behind is only litter.
        let _ = fs::remove_file(&temporary);
    }

    written.map(|()| true)
}

/// Whether a document on disk is `document`, a CRLF read as a LF: a document committed and checked out with
/// `core.autocrlf`, as on Windows, is the same.
fn same(written: &[u8], document: &[u8]) -> bool {
    lf(written) == lf(document)
}

fn lf(bytes: &[u8]) -> Vec<u8> {
    bytes
        .iter()
        .enumerate()
        .filter(|&(index, &byte)| !(byte == b'\r' && bytes.get(index + 1) == Some(&b'\n')))
        .map(|(_, &byte)| byte)
        .collect()
}

/// The key of `api`'s output, which its errors name.
pub(crate) fn output(api: &Api) -> String {
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
    fn a_document_checked_out_with_crlf_is_the_same() {
        let dir = tempfile::tempdir().expect("a temporary directory");
        let path = dir.path().join("openapi.json");

        fs::write(&path, b"{\r\n  \"openapi\": \"3.1.0\"\r\n}\r\n")
            .expect("the document is written");

        assert!(!write(&path, b"{\n  \"openapi\": \"3.1.0\"\n}\n").expect("the document is read"));
        assert_eq!(
            fs::read(&path).expect("the document is there"),
            b"{\r\n  \"openapi\": \"3.1.0\"\r\n}\r\n"
        );
    }

    #[test]
    fn only_a_crlf_is_read_as_a_lf() {
        assert!(same(b"a\r\nb\r\n", b"a\nb\n"));
        assert!(!same(b"a\rb\n", b"a\nb\n"));
        assert!(!same(b"a\r\n\r\n", b"a\n"));
    }

    /// An open document keeps what it held: the new one is another file, renamed over it, not the old one truncated
    /// and written again. A Unix file is read once renamed over, which Windows refuses.
    #[cfg(unix)]
    #[test]
    fn a_document_is_renamed_over_the_old_one() {
        use std::io::Read;

        let dir = tempfile::tempdir().expect("a temporary directory");
        let path = dir.path().join("openapi.json");

        fs::write(&path, b"old").expect("the document is written");
        let mut old = fs::File::open(&path).expect("the document opens");

        assert!(write(&path, b"new").expect("the document is written"));

        let mut read = String::new();

        old.read_to_string(&mut read)
            .expect("the old document is read");
        assert_eq!(read, "old");
        assert_eq!(fs::read(&path).expect("the document is there"), b"new");

        let files: Vec<_> = fs::read_dir(dir.path())
            .expect("the directory is read")
            .map(|entry| entry.expect("the directory is read").file_name())
            .collect();
        assert_eq!(files, ["openapi.json"], "no temporary file is left");
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
