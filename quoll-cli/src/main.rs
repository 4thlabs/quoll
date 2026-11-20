//! `cargo quoll`: writes the OpenAPI documents of a workspace's contracts, runs the commands that follow them, and
//! builds its web side, as the workspace's `[workspace.metadata.quoll]` says.

mod cargo;
mod commands;
mod config;
mod documents;

use std::process::ExitCode;

use anyhow::{Context, Result};
use clap::{Args, Parser, Subcommand};

use crate::cargo::Workspace;

/// Cargo runs `cargo quoll gen` as `cargo-quoll quoll gen`.
#[derive(Parser)]
#[command(name = "cargo", bin_name = "cargo")]
enum Cli {
    Quoll(Quoll),
}

/// The OpenAPI documents of the workspace's contracts, and its web side.
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
