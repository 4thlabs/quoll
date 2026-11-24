# A command line on the Docker Engine API

A contract of the [Docker Engine API](https://docs.docker.com/reference/api/engine/), an API that quoll did not write,
and a command line that calls it.

```text
docker/
  api/   docker-api   the contract and its models: quoll and serde, `#[server(none)]`, as the daemon is only called
  cli/   docker-cli   the command line: clap, and docker-api with feature "client"
```

## Running it

This directory is its own workspace, with its own `Cargo.lock`. From it:

```bash
cargo run -p docker-cli -- ps -a
cargo run -p docker-cli -- inspect web
cargo run -p docker-cli -- start web
cargo run -p docker-cli -- stop web -t 5
cargo run -p docker-cli -- logs -f web
cargo run -p docker-cli -- events --since 1763632800
```

Like the `docker` command, it reaches the daemon that `DOCKER_HOST` names:

- `unix:///path/to/docker.sock`;
- `tcp://host:port`, without TLS, on port 2375 when it names no port.

When `DOCKER_HOST` names none, it reaches Docker's own socket, `/var/run/docker.sock`.

TLS, `DOCKER_TLS_VERIFY` and `ssh://` are not supported.

## How the contract meets Docker

- **The version.** The api's prefix is `/v1.47`, which Docker 27.3 and later serve. A daemon that is too old answers
  400, with a message that says so.
- **The socket.** The client uses reqwest's own `unix_socket`, given to `client::builder().http(..)`. The URL is then
  `http://localhost`. Docker does not read its host.
- **The errors.** Docker answers `{"message": "..."}`, not an RFC 9457 problem. The api says so with
  `#[error(DockerError)]`. Each call's error then has that message as its `detail()`, and shows it:
  `another api answered 404: No such container: web`.
- **304.** `start` and `stop` answer `304 Not Modified` when the container is already running, or already stopped.
  The command line says so rather than fail.
- **The logs.** A `ByteStream`, read as its chunks come. Without a TTY, Docker sends frames
  (`application/vnd.docker.multiplexed-stream`). Each frame is a header of 8 bytes, then its bytes. The header holds
  the stream, three zeros and the size. The command line splits the frames, across chunks, and writes stdout and
  stderr each to its own stream. With a TTY (`application/vnd.docker.raw-stream`), it writes the bytes as they come.
- **The events.** `JsonLines<Event>`. Docker 29 and later send them as `application/jsonl`, and older versions as
  `application/json`, which quoll's client also reads as JSON Lines. quoll's client reads the progress of a pull the
  same way. The command line skips an event of a kind newer than itself, which it does not read, with a warning on
  stderr. The stream goes on.

## Its tests

`cli/tests/cli.rs` runs each command against a fake daemon written with axum, on port 0 and on a Unix socket.
`cli/tests/daemon.rs` runs them against a real daemon: it pulls `alpine:3`, makes a container, then starts it, reads
its logs and its events, stops it and removes it. When nothing listens where the daemon would, the test says it is
skipped and passes, unless `DOCKER_CLI_TEST_DAEMON=required` is set. CI sets it on its runners, where Docker runs.
`cargo test` in this directory runs both.
