//! `cargo quoll`: writes the OpenAPI documents of a workspace's contracts, runs the commands that follow them, builds
//! its web side, and runs its dev loop, as the workspace's `[workspace.metadata.quoll]` says.

mod cargo;
mod commands;
mod config;
mod dev;
mod documents;

use std::process::ExitCode;

use anyhow::{Context, Result};
use clap::{Args, Parser, Subcommand};

use crate::cargo::Workspace;

// Cargo runs `cargo quoll gen` as `cargo-quoll quoll gen`.
#[derive(Parser)]
#[command(name = "cargo", bin_name = "cargo")]
enum Cli {
    Quoll(Quoll),
}

/// Writes the OpenAPI documents of the workspace's contracts, runs the commands that follow them, builds its web side,
/// and runs its dev loop, as `[workspace.metadata.quoll]` says in the workspace's Cargo.toml.
#[derive(Args)]
#[command(version)]
struct Quoll {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Writes each api's document when it changed, then runs its `after` commands.
    Gen {
        /// Writes nothing, and fails when a document on disk is not up to date.
        #[arg(long)]
        check: bool,
    },
    /// Runs `gen`, then `web.build`.
    Build,
    /// Runs `gen`, starts the servers, then `web.dev`, and makes again the documents and the servers a change touches,
    /// until Ctrl-C.
    Dev {
        /// Prints one JSON line per event on stdout, the output for a person going to stderr.
        #[arg(long)]
        json: bool,
    },
}

fn main() -> ExitCode {
    let Cli::Quoll(quoll) = Cli::parse();

    match run(quoll.command) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error:#}");
            ExitCode::FAILURE
        }
    }
}

fn run(command: Command) -> Result<()> {
    let workspace = Workspace::load()?;

    match command {
        Command::Gen { check: false } => documents::generate(&workspace),
        Command::Gen { check: true } => documents::check(&workspace),
        Command::Build => build(&workspace),
        Command::Dev { json } => dev::run(&workspace, json),
    }
}

fn build(workspace: &Workspace) -> Result<()> {
    let web = workspace
        .config
        .web
        .as_ref()
        .context("workspace.metadata.quoll.web: missing, with the `build` command that `cargo quoll build` runs")?;
    let build = web.build.as_ref().with_context(|| {
        format!(
            "{}.build: missing, the command that `cargo quoll build` runs",
            web.key
        )
    })?;

    documents::generate(workspace)?;

    build.run(&workspace.root.join(&web.dir))
}
