//! `cargo quoll gen` and `build`, run as cargo runs them, on a small workspace copied for each test: a contract whose
//! binary prints its document, and `record`, a command that writes down each of its runs.

use std::env;
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::time::{Duration, SystemTime};

use tempfile::TempDir;

/// What the contract's binary prints.
const DOCUMENT: &str = "{\"openapi\": \"3.1.0\"}\n";

/// The config of most tests: one api, whose document two commands follow, and a web side that builds.
const CONFIG: &str = r#"
[[workspace.metadata.quoll.api]]
package = "contract"
bin = "openapi"
output = "web/openapi.json"
after = [["record", "first"], ["record", "second"]]

[workspace.metadata.quoll.web]
dir = "web"
build = ["record", "build"]
"#;

/// A copy of the workspace under `tests/fixture`, with its binaries built.
struct Fixture {
    dir: TempDir,
}

impl Fixture {
    /// The workspace, with `config` written after its `[workspace]` table.
    fn new(config: &str) -> Self {
        let dir = tempfile::tempdir().expect("a temporary directory");

        copy_dir(
            &Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixture"),
            dir.path(),
        );
        fs::write(
            dir.path().join("Cargo.toml"),
            format!("[workspace]\nmembers = [\"contract\"]\nresolver = \"3\"\n{config}"),
        )
        .expect("the workspace's Cargo.toml is written");

        // `record` is found on the PATH, as a project's own commands are: it is built first.
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

    fn path(&self, relative: &str) -> PathBuf {
        self.dir.path().join(relative)
    }

    /// Runs `cargo quoll` with `args`, in the workspace's root, with its binaries on the PATH.
    fn cargo_quoll(&self, args: &[&str]) -> Output {
        let path = env::var_os("PATH").unwrap_or_default();
        let path = env::join_paths(
            std::iter::once(self.path("target/debug")).chain(env::split_paths(&path)),
        )
        .expect("the PATH joins");

        Command::new(env!("CARGO_BIN_EXE_cargo-quoll"))
            .arg("quoll")
            .args(args)
            .current_dir(self.dir.path())
            .env("PATH", path)
            .env_remove("CARGO_TARGET_DIR")
            .env_remove("CARGO_BUILD_TARGET_DIR")
            .output()
            .expect("cargo-quoll runs")
    }

    /// What `record` wrote down in `dir`, nothing when it did not run there.
    fn record(&self, dir: &str) -> String {
        fs::read_to_string(self.path(dir).join("record.log")).unwrap_or_default()
    }

    fn document(&self) -> Option<String> {
        fs::read_to_string(self.path("web/openapi.json")).ok()
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

fn stderr(output: &Output) -> String {
    String::from_utf8_lossy(&output.stderr).into_owned()
}

#[track_caller]
fn assert_succeeded(output: &Output) {
    assert!(
        output.status.success(),
        "cargo quoll failed: {}",
        stderr(output)
    );
}

#[track_caller]
fn assert_failed_with(output: &Output, messages: &[&str]) {
    let stderr = stderr(output);

    assert!(!output.status.success(), "cargo quoll succeeded: {stderr}");
    for message in messages {
        assert!(
            stderr.contains(message),
            "`{message}` is not in the output: {stderr}"
        );
    }
}

#[test]
fn gen_writes_the_document_then_runs_after_in_order_in_its_directory() {
    let fixture = Fixture::new(CONFIG);

    assert_succeeded(&fixture.cargo_quoll(&["gen"]));

    assert_eq!(fixture.document().as_deref(), Some(DOCUMENT));
    assert_eq!(fixture.record("web"), "first\nsecond\n");
    assert_eq!(
        fixture.record("."),
        "",
        "after runs in the directory of output"
    );
}

#[test]
fn gen_leaves_a_document_that_did_not_change_and_runs_after_again() {
    let fixture = Fixture::new(CONFIG);

    assert_succeeded(&fixture.cargo_quoll(&["gen"]));

    let long_ago = SystemTime::UNIX_EPOCH + Duration::from_secs(86_400);

    File::options()
        .write(true)
        .open(fixture.path("web/openapi.json"))
        .and_then(|file| file.set_modified(long_ago))
        .expect("the document's time is set");

    assert_succeeded(&fixture.cargo_quoll(&["gen"]));

    let modified = fs::metadata(fixture.path("web/openapi.json"))
        .and_then(|metadata| metadata.modified())
        .expect("the document's time is read");
    assert_eq!(modified, long_ago, "the document is not written again");
    assert_eq!(fixture.record("web"), "first\nsecond\nfirst\nsecond\n");
}

#[test]
fn gen_writes_a_document_that_changed() {
    let fixture = Fixture::new(CONFIG);

    fs::create_dir(fixture.path("web")).expect("the directory is made");
    fs::write(fixture.path("web/openapi.json"), "stale").expect("the document is written");

    assert_succeeded(&fixture.cargo_quoll(&["gen"]));

    assert_eq!(fixture.document().as_deref(), Some(DOCUMENT));
}

#[test]
fn a_failing_command_fails_gen_with_its_output() {
    let fixture = Fixture::new(&CONFIG.replace(
        r#"after = [["record", "first"], ["record", "second"]]"#,
        r#"after = [["record", "fail"], ["record", "never"]]"#,
    ));

    let output = fixture.cargo_quoll(&["gen"]);

    assert_failed_with(
        &output,
        &[
            "record: failing, as asked",
            "workspace.metadata.quoll.api[0].after[0]: `record fail` failed",
        ],
    );
    assert_eq!(
        fixture.record("web"),
        "",
        "the commands after it do not run"
    );
}

#[test]
fn a_program_not_on_the_path_fails_gen() {
    let fixture = Fixture::new(&CONFIG.replace(
        r#"after = [["record", "first"], ["record", "second"]]"#,
        r#"after = [["quoll-no-such-program"]]"#,
    ));

    let output = fixture.cargo_quoll(&["gen"]);

    assert_failed_with(
        &output,
        &[
            "workspace.metadata.quoll.api[0].after[0]: cannot find the program `quoll-no-such-program` on the PATH",
        ],
    );
}

#[test]
fn a_contract_that_does_not_compile_fails_gen_with_cargo_s_output() {
    let fixture = Fixture::new(CONFIG);

    fs::write(
        fixture.path("contract/src/bin/openapi.rs"),
        "fn main() { not_rust }\n",
    )
    .expect("the binary is broken");

    let output = fixture.cargo_quoll(&["gen"]);

    assert_failed_with(
        &output,
        &[
            "cannot find value `not_rust`",
            "workspace.metadata.quoll.api[0]: cargo cannot build the binary `openapi` of `contract`",
        ],
    );
    assert_eq!(fixture.document(), None);
    assert_eq!(fixture.record("web"), "");
}

#[test]
fn a_config_that_cannot_be_read_names_its_key() {
    let fixture = Fixture::new(&CONFIG.replace("output = \"web/openapi.json\"\n", ""));

    let output = fixture.cargo_quoll(&["gen"]);

    assert_failed_with(
        &output,
        &["workspace.metadata.quoll.api[0].output: missing"],
    );
}

#[test]
fn check_passes_on_a_document_up_to_date_and_runs_nothing() {
    let fixture = Fixture::new(CONFIG);

    assert_succeeded(&fixture.cargo_quoll(&["gen"]));

    assert_succeeded(&fixture.cargo_quoll(&["gen", "--check"]));

    assert_eq!(
        fixture.record("web"),
        "first\nsecond\n",
        "check runs no after"
    );
}

#[test]
fn check_fails_on_a_stale_document_and_writes_nothing() {
    let fixture = Fixture::new(CONFIG);

    fs::create_dir(fixture.path("web")).expect("the directory is made");
    fs::write(fixture.path("web/openapi.json"), "stale").expect("the document is written");

    let output = fixture.cargo_quoll(&["gen", "--check"]);

    assert_failed_with(&output, &["web/openapi.json: not the one `openapi` prints"]);
    assert_eq!(fixture.document().as_deref(), Some("stale"));
    assert_eq!(fixture.record("web"), "");
}

#[test]
fn a_document_checked_out_with_crlf_is_up_to_date() {
    let fixture = Fixture::new(CONFIG);
    let crlf = DOCUMENT.replace('\n', "\r\n");

    fs::create_dir(fixture.path("web")).expect("the directory is made");
    fs::write(fixture.path("web/openapi.json"), &crlf).expect("the document is written");

    assert_succeeded(&fixture.cargo_quoll(&["gen", "--check"]));
    assert_succeeded(&fixture.cargo_quoll(&["gen"]));

    assert_eq!(
        fixture.document(),
        Some(crlf),
        "the document is not written again"
    );
}

#[test]
fn check_does_not_update_the_cargo_lock() {
    let fixture = Fixture::new(CONFIG);

    fs::remove_file(fixture.path("Cargo.lock")).expect("the fixture's build wrote its Cargo.lock");

    let output = fixture.cargo_quoll(&["gen", "--check"]);

    assert_failed_with(&output, &["--locked"]);
    assert!(!fixture.path("Cargo.lock").exists());
}

#[test]
fn check_fails_on_a_missing_document_and_writes_nothing() {
    let fixture = Fixture::new(CONFIG);

    let output = fixture.cargo_quoll(&["gen", "--check"]);

    assert_failed_with(&output, &["web/openapi.json: missing"]);
    assert_eq!(fixture.document(), None);
}

#[test]
fn build_runs_gen_then_web_build_in_its_directory() {
    let fixture = Fixture::new(CONFIG);

    assert_succeeded(&fixture.cargo_quoll(&["build"]));

    assert_eq!(fixture.document().as_deref(), Some(DOCUMENT));
    assert_eq!(fixture.record("web"), "first\nsecond\nbuild\n");
}

#[test]
fn build_with_no_web_fails_before_gen() {
    let config = CONFIG
        .split_once("[workspace.metadata.quoll.web]")
        .expect("the config has a web side")
        .0;
    let fixture = Fixture::new(config);

    let output = fixture.cargo_quoll(&["build"]);

    assert_failed_with(&output, &["workspace.metadata.quoll.web: missing"]);
    assert_eq!(fixture.document(), None);
}
