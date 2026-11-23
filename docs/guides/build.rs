//! Writes the README of the repository for the doctests, with the preludes its short blocks need.
//!
//! A Rust block of the README that follows a comment `<!-- doctest NAME: WORD... -->` takes, before its own lines and
//! hidden as rustdoc hides `# ` lines, the code of each word in turn: a word ending in `.rs` is a file of `preludes/`,
//! any other the lines of a block named so before it, without its own preludes. GitHub shows neither the comment nor
//! the preludes, so that a block shows only what it is about, and compiles all the same.

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let readme = manifest.join("../../README.md");
    let preludes = manifest.join("preludes");

    println!("cargo::rerun-if-changed={}", readme.display());
    println!("cargo::rerun-if-changed={}", preludes.display());

    let written = with_preludes(&fs::read_to_string(&readme)?, &preludes)?;
    let out = PathBuf::from(env::var("OUT_DIR")?);

    fs::write(out.join("README.md"), written)?;
    Ok(())
}

/// The Markdown, each block that follows a `doctest` comment preceded by its preludes, hidden.
fn with_preludes(markdown: &str, preludes: &Path) -> Result<String, Box<dyn std::error::Error>> {
    let mut written = String::new();
    let mut named = HashMap::new();
    let mut lines = markdown.lines();

    while let Some(line) = lines.next() {
        written.push_str(line);
        written.push('\n');

        let Some((name, words)) = doctest(line) else {
            continue;
        };

        let fence = lines.next().unwrap_or_default();

        if !fence.starts_with("```rust") {
            return Err(format!("`{line}` is not followed by a Rust block").into());
        }

        written.push_str(fence);
        written.push('\n');

        for word in words.split_whitespace() {
            let code = prelude(word, preludes, &named)
                .map_err(|error| format!("`{line}` names `{word}`: {error}"))?;

            for hidden in code.lines() {
                written.push_str(if hidden.is_empty() { "#" } else { "# " });
                written.push_str(hidden);
                written.push('\n');
            }
        }

        let mut code = String::new();

        for line in lines.by_ref() {
            written.push_str(line);
            written.push('\n');

            if line == "```" {
                break;
            }

            code.push_str(line);
            code.push('\n');
        }

        named.insert(name.to_owned(), code);
    }

    Ok(written)
}

/// The code a word of a `doctest` comment names: a file of `preludes/`, or a block named before it.
fn prelude(
    word: &str,
    preludes: &Path,
    named: &HashMap<String, String>,
) -> Result<String, Box<dyn std::error::Error>> {
    if word.ends_with(".rs") {
        return Ok(fs::read_to_string(preludes.join(word))?);
    }

    let code = named.get(word).ok_or("no block before it is named so")?;

    Ok(code.clone())
}

/// The name of a `<!-- doctest NAME: WORD... -->` comment, and its words.
fn doctest(line: &str) -> Option<(&str, &str)> {
    line.strip_prefix("<!-- doctest ")?
        .strip_suffix(" -->")?
        .split_once(':')
}
