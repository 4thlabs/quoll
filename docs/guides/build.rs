//! Writes the README of the repository for the doctests, with the preludes its short blocks need.
//!
//! A Rust block of the README that follows a comment `<!-- doctest NAME: WORD... -->` begins with the code of each
//! word in turn: a word ending in `.rs` is a file of `preludes/`, any other a block named so before it, without its
//! own preludes. GitHub shows neither the comment nor the preludes, so that a block shows only what it is about, and
//! compiles all the same.
//!
//! The preludes are one hidden line, a module of `include!`, written in the place of the comment, so that the lines of every
//! block keep their numbers in the README: a doctest that fails is named by the line of its comment, and its errors
//! point at the README's own lines.

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

type Error = Box<dyn std::error::Error>;

fn main() -> Result<(), Error> {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let readme = manifest.join("../../README.md");
    let preludes = manifest.join("preludes");
    let out = PathBuf::from(env::var("OUT_DIR")?);

    println!("cargo::rerun-if-changed={}", readme.display());
    println!("cargo::rerun-if-changed={}", preludes.display());

    let written = with_preludes(&fs::read_to_string(&readme)?, &preludes, &out)?;

    fs::write(out.join("README.md"), written)?;
    Ok(())
}

/// The Markdown, each block that follows a `doctest` comment beginning with its preludes, hidden. The code of each
/// such block is written to `out`, as `NAME.rs`, for the blocks after it.
fn with_preludes(markdown: &str, preludes: &Path, out: &Path) -> Result<String, Error> {
    let mut written = String::new();
    let mut named = HashMap::new();
    let mut lines = markdown.lines();

    while let Some(line) = lines.next() {
        let Some((name, words)) = doctest(line) else {
            written.push_str(line);
            written.push('\n');
            continue;
        };

        if named.contains_key(name) {
            return Err(format!("`{line}`: a block before it is named `{name}` already").into());
        }

        let fence = lines.next().unwrap_or_default();

        if !fence.starts_with("```rust") {
            return Err(format!("`{line}` is not followed by a Rust block").into());
        }

        let mut included = Vec::new();

        for word in words.split_whitespace() {
            let path = prelude(word, preludes, &named)
                .map_err(|error| format!("`{line}` names `{word}`: {error}"))?;

            included.push(format!("include!({:?});", path.display().to_string()));
        }

        // The fence takes the comment's line, and the preludes the fence's: the code keeps its lines. A module holds
        // them: rustdoc puts a block without `main` in a function, where `include!` reads no items.
        written.push_str(fence);
        written.push('\n');
        written.push_str(&format!(
            "# mod preludes {{ {} }} use preludes::*;\n",
            included.join(" ")
        ));

        let code =
            block(&mut lines, fence, &mut written).map_err(|error| format!("`{line}`: {error}"))?;
        let path = out.join(format!("{name}.rs"));

        fs::write(&path, code)?;
        named.insert(name.to_owned(), path);
    }

    Ok(written)
}

/// The code of a block, up to the fence that closes `fence`, written to `written` with that fence.
fn block<'a>(
    lines: &mut impl Iterator<Item = &'a str>,
    fence: &str,
    written: &mut String,
) -> Result<String, Error> {
    let ticks = fence.len() - fence.trim_start_matches('`').len();
    let mut code = String::new();

    for line in lines {
        written.push_str(line);
        written.push('\n');

        if closes(line, ticks) {
            return Ok(code);
        }

        // rustdoc would hide it, and GitHub show it: what a block hides goes in its preludes.
        let hidden = line.trim_start();

        if hidden == "#" || hidden.starts_with("# ") {
            return Err(format!("its line `{line}` is one rustdoc hides").into());
        }

        code.push_str(line);
        code.push('\n');
    }

    Err("its block is not closed".into())
}

/// Whether `line` closes a fence of `ticks` backticks: as many or more, indented by three spaces at most.
fn closes(line: &str, ticks: usize) -> bool {
    let fence = line.trim();
    let indent = line.len() - line.trim_start().len();

    indent <= 3 && fence.len() >= ticks && fence.bytes().all(|byte| byte == b'`')
}

/// The file a word of a `doctest` comment names: one of `preludes/`, or that of a block named before it.
fn prelude(
    word: &str,
    preludes: &Path,
    named: &HashMap<String, PathBuf>,
) -> Result<PathBuf, Error> {
    if word.ends_with(".rs") {
        let path = preludes.join(word);

        fs::metadata(&path)?;
        return Ok(path);
    }

    let path = named.get(word).ok_or("no block before it is named so")?;

    Ok(path.clone())
}

/// The name of a `<!-- doctest NAME: WORD... -->` comment, and its words.
fn doctest(line: &str) -> Option<(&str, &str)> {
    line.strip_prefix("<!-- doctest ")?
        .strip_suffix(" -->")?
        .split_once(':')
}
