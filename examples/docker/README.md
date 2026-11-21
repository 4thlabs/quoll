# A command line on the Docker Engine API

A contract of an API that quoll did not write, the [Docker Engine API](https://docs.docker.com/reference/api/engine/),
and a command line that calls it.

```text
docker/
  api/   docker-api   the contract and its models: quoll and serde, `#[openapi(none)]`, as the daemon is only called
  cli/   docker-cli   the command line: clap, and docker-api with feature "client"
```

## Running it

From this directory, a workspace of its own, with its `Cargo.lock`:

```bash
cargo run -p docker-cli -- ps -a
cargo run -p docker-cli -- inspect web
cargo run -p docker-cli -- start web
cargo run -p docker-cli -- stop web -t 5
cargo run -p docker-cli -- logs -f web
cargo run -p docker-cli -- events --since 1763632800
```

It reaches the daemon `DOCKER_HOST` names, as the `docker` command does: `unix:///path/to/docker.sock`, or
`tcp://host:port` without TLS, 2375 when it names no port; and Docker's own socket, `/var/run/docker.sock`, when it
names none. TLS, `DOCKER_TLS_VERIFY`, and `ssh://` are not supported.

## How the contract meets Docker

- **The version.** The api's prefix is `/v1.47`, which Docker 27.3 and later serve. A daemon too old answers 400, with
  a message that says so.
- **The socket.** reqwest's own `unix_socket`, given to `client::builder().http(..)`; the URL is then
  `http://localhost`, whose host Docker does not read.
- **The errors.** Docker answers `{"message": "..."}`, not an RFC 9457 problem: the command line reads it with
  `Error::body_as::<DockerError>()`.
- **304.** `start` and `stop` answer `304 Not Modified` when the container runs, or is stopped, already: the command
  line says so rather than fail.
- **The logs.** A `ByteStream`, read as its chunks come. Without a TTY, Docker sends frames,
  `application/vnd.docker.multiplexed-stream`, each a header of 8 bytes, the stream, three zeros and the size, before
  its bytes: the command line splits them, across chunks, and writes stdout and stderr each to its own. With a TTY,
  `application/vnd.docker.raw-stream`, it writes them as they come.
- **The events.** `JsonLines<Event>`: Docker 29 and later send them as `application/jsonl`, older ones as
  `application/json`, which quoll's client reads as JSON Lines too. So does the progress of a pull. An event the
  command line does not read, of a kind newer than it, is skipped with a warning on stderr, and the stream goes on.

## Its tests

`cli/tests/cli.rs` runs each command against a fake daemon written with axum, on port 0 and on a Unix socket.
`cli/tests/daemon.rs` runs them against a real daemon: it pulls `alpine:3`, makes a container, then starts it, reads
its logs and its events, stops it and removes it. With nothing listening where the daemon would, it says it is skipped and passes, unless
`DOCKER_CLI_TEST_DAEMON=required`, which CI sets on its runners, where Docker runs. `cargo test` in this directory
runs both.
