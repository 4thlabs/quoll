//! `docker-cli`: a command line of the Docker Engine API, written on the contract of `docker-api`.

mod args;
mod commands;
mod frames;
mod host;
mod utils;

use clap::Parser;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let args = args::Cli::parse();
    let docker = host::Host::from_env()?.client()?;

    commands::run(&docker, args.command).await
}
