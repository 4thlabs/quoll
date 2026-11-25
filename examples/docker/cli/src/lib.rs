//! `docker-cli`: a command line of the Docker Engine API, written on the contract of `docker-api`.

mod args;
mod commands;
mod frames;
mod host;
mod utils;

pub use args::{Cli, Command};
pub use commands::run;
pub use frames::{Demux, FrameError, Output, Piece};
pub use host::Host;
