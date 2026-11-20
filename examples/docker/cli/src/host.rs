//! Where the daemon listens, as `DOCKER_HOST` says it, and the client that reaches it there.

use std::path::{Path, PathBuf};

use anyhow::{bail, ensure};
use docker_api::{Docker, DockerClient};
use quoll::client;

/// Docker's own socket, where `DOCKER_HOST` names none.
const DEFAULT_SOCKET: &str = "/var/run/docker.sock";

/// The port of a daemon on TCP without TLS, where `DOCKER_HOST` names none.
const DEFAULT_PORT: u16 = 2375;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Host {
    /// A Unix socket.
    Unix(PathBuf),
    /// The URL of a daemon on TCP.
    Tcp(String),
}

impl Host {
    /// The daemon `DOCKER_HOST` names.
    pub fn from_env() -> anyhow::Result<Self> {
        // TLS would need the client's certificate, which this example does not send: better said than a failed call.
        if std::env::var_os("DOCKER_TLS_VERIFY").is_some_and(|value| !value.is_empty()) {
            bail!("DOCKER_TLS_VERIFY is set: this command line reaches a daemon without TLS only");
        }

        Self::parse(std::env::var("DOCKER_HOST").ok().as_deref())
    }

    /// The daemon `docker_host` names, as `DOCKER_HOST` is written: `unix:///path`, `tcp://host:port`, or nothing.
    pub fn parse(docker_host: Option<&str>) -> anyhow::Result<Self> {
        let Some(value) = docker_host.filter(|value| !value.is_empty()) else {
            return Ok(Self::Unix(DEFAULT_SOCKET.into()));
        };

        if let Some(path) = value.strip_prefix("unix://") {
            ensure!(
                !path.is_empty(),
                "DOCKER_HOST is `{value}`, which names no socket: write `unix:///path/to/docker.sock`"
            );

            return Ok(Self::Unix(path.into()));
        }

        if let Some(address) = value.strip_prefix("tcp://") {
            let address = address.trim_end_matches('/');

            ensure!(
                !address.is_empty(),
                "DOCKER_HOST is `{value}`, which names no host: write `tcp://host:port`"
            );

            // An IPv6 address holds `:` too, but within its brackets.
            let has_port = address
                .rsplit_once(':')
                .is_some_and(|(_, port)| !port.contains(']'));
            let url = if has_port {
                format!("http://{address}")
            } else {
                format!("http://{address}:{DEFAULT_PORT}")
            };

            return Ok(Self::Tcp(url));
        }

        bail!(
            "DOCKER_HOST is `{value}`, which this command line does not reach: write `unix:///path` or `tcp://host:port`"
        )
    }

    /// A client of the daemon.
    pub fn client(&self) -> anyhow::Result<DockerClient> {
        let builder = client::builder().with(client::user_agent(concat!(
            "docker-cli/",
            env!("CARGO_PKG_VERSION")
        )));

        match self {
            Self::Tcp(url) => Ok(builder.build::<Docker>(url)?),
            Self::Unix(path) => over_socket(builder, path),
        }
    }
}

#[cfg(unix)]
fn over_socket(builder: client::Builder, path: &Path) -> anyhow::Result<DockerClient> {
    let path = path.to_owned();

    // Over the socket, the URL's host is only the request's `Host`, which Docker does not read.
    Ok(builder
        .http(move |http| http.unix_socket(path))
        .build::<Docker>("http://localhost")?)
}

#[cfg(not(unix))]
fn over_socket(_: client::Builder, path: &Path) -> anyhow::Result<DockerClient> {
    bail!(
        "the socket {} needs a Unix system: set DOCKER_HOST=tcp://host:port",
        path.display()
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parsed(docker_host: Option<&str>) -> Host {
        Host::parse(docker_host).expect("a host")
    }

    #[test]
    fn no_host_is_dockers_own_socket() {
        assert_eq!(parsed(None), Host::Unix("/var/run/docker.sock".into()));
        assert_eq!(parsed(Some("")), Host::Unix("/var/run/docker.sock".into()));
    }

    #[test]
    fn a_unix_host_is_its_socket() {
        assert_eq!(
            parsed(Some("unix:///run/user/1000/docker.sock")),
            Host::Unix("/run/user/1000/docker.sock".into())
        );
    }

    #[test]
    fn a_tcp_host_is_an_http_url_on_2375_unless_it_names_its_port() {
        assert_eq!(
            parsed(Some("tcp://127.0.0.1:2376/")),
            Host::Tcp("http://127.0.0.1:2376".into())
        );
        assert_eq!(
            parsed(Some("tcp://docker")),
            Host::Tcp("http://docker:2375".into())
        );
        assert_eq!(
            parsed(Some("tcp://[::1]")),
            Host::Tcp("http://[::1]:2375".into())
        );
        assert_eq!(
            parsed(Some("tcp://[::1]:2380")),
            Host::Tcp("http://[::1]:2380".into())
        );
    }

    #[test]
    fn another_host_says_what_to_write() {
        for docker_host in [
            "ssh://me@host",
            "npipe:////./pipe/docker_engine",
            "unix://",
            "tcp://",
        ] {
            let error = Host::parse(Some(docker_host)).expect_err("not a host");

            assert!(error.to_string().contains("write"), "{error}");
        }
    }
}
