use clap::{Parser, Subcommand};

/// A command line of the Docker Engine API, on the daemon `DOCKER_HOST` names, `unix:///path` or `tcp://host:port`,
/// or on `/var/run/docker.sock` when it names none.
#[derive(Debug, Parser)]
#[command(name = "docker-cli", version)]
pub struct Cli {
    #[command(subcommand)]
    pub command: Command,
}

#[derive(Debug, Subcommand)]
pub enum Command {
    /// Lists the running containers.
    Ps {
        /// Lists every container, the stopped ones too.
        #[arg(short, long)]
        all: bool,
    },
    /// Shows a container, as JSON.
    Inspect { container: String },
    /// Starts a container.
    Start { container: String },
    /// Stops a container.
    Stop {
        container: String,
        /// Seconds to wait before it is killed: the container's own setting when missing.
        #[arg(short, long)]
        timeout: Option<i32>,
    },
    /// Writes what a container wrote, its standard output and its standard error each to the command line's own.
    Logs {
        container: String,
        /// Goes on with what it writes next, until it stops.
        #[arg(short, long)]
        follow: bool,
    },
    /// Writes the daemon's events as they happen, one a line.
    Events,
}
