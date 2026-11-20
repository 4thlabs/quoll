use clap::Parser;
use docker_cli::{Cli, Host};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    let docker = Host::from_env()?.client()?;

    docker_cli::run(&docker, cli.command).await
}
