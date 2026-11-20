//! `cargo quoll gen` on the workspace of quoll itself, whose config names the notes example's contract.

use std::path::Path;
use std::process::{Command, Output};

use serde_json::Value;

fn cargo_quoll(root: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_cargo-quoll"))
        .arg("quoll")
        .args(args)
        .current_dir(root)
        .output()
        .expect("cargo-quoll runs")
}

#[track_caller]
fn assert_succeeded(output: &Output) {
    assert!(
        output.status.success(),
        "cargo quoll failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn gen_writes_the_document_of_the_notes_example() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("quoll-cli is in the workspace's root");

    assert_succeeded(&cargo_quoll(root, &["gen"]));

    let read = |path: &str| -> Value {
        let text = std::fs::read_to_string(root.join(path)).expect("the document is there");
        serde_json::from_str(&text).expect("the document is JSON")
    };
    // Compared as JSON, as the snapshot test of the example does: the order of the keys is serde_json's.
    assert!(
        read("examples/notes/web/openapi.json")
            == read("examples/notes/api/tests/openapi.snapshot.json"),
        "the document is not the example's snapshot"
    );

    assert_succeeded(&cargo_quoll(root, &["gen", "--check"]));
}
