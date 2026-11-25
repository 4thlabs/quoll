use std::process::ExitCode;

use clap::Parser;
use docker_cli::{Cli, Host};

fn main() -> anyhow::Result<ExitCode> {
    let cli = Cli::parse();
    let runtime = tokio::runtime::Runtime::new()?;
    let ran = runtime.block_on(async {
        let docker = Host::from_env()?.client()?;

        docker_cli::run(&docker, cli.command).await
    });

    // `exec` reads the command line's input on a thread of tokio's, a read that cannot be cancelled: once the command
    // has ended, the runtime does not wait for a line that may never come.
    runtime.shutdown_background();
    ran
}
