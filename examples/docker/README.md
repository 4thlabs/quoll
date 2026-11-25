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
echo hello | cargo run -p docker-cli -- exec web cat
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
  Docker reports an error inside the logs in two ways, and the command line shows its message after the output
  before it. One is a frame of stream 3, whose bytes are the message. The other is text where a header should be,
  such as `Error grabbing logs: ...`, whose first line is the message.
- **exec.** `docker-cli exec <container> <command>...` makes the command with `POST /containers/{id}/exec`, then
  starts it with `POST /exec/{id}/start`, a route `-> TcpTunnel`. Docker answers `101 Switching Protocols`, and the
  connection carries the command's input and output. The command line copies its own input into the tunnel, and
  closes the half it writes when its input ends, which Docker passes on as the end of the command's input, as `cat`
  needs. It reads the output as it reads the logs: frames without a TTY, the bytes as they are with `-t`, as the
  content type of the 101 says. Once the output ends, it reads the command's code with `GET /exec/{id}/json` and
  exits with it, as `docker exec` does. Its runtime does not wait, at the end, for a read of its own input that may
  never end. `resize` sets the size of a TTY, which the test against a real daemon checks with `stty size`.
- **The events.** `JsonLines<Event>`. Docker 29 and later send them as `application/jsonl`, and older versions as
  `application/json`, which quoll's client also reads as JSON Lines. quoll's client reads the progress of a pull the
  same way. The command line skips an event of a kind newer than itself, which it does not read, with a warning on
  stderr. The stream goes on.

## Its tests

`cli/tests/cli.rs` runs each command against a fake daemon written with axum, on port 0 and on a Unix socket.
`cli/tests/daemon.rs` runs them against a real daemon: it pulls `alpine:3`, makes a container, then starts it, reads
its logs and its events, runs `cat` and a shell in it, stops it and removes it. When nothing listens where the daemon would, the test says it is
skipped and passes, unless `DOCKER_CLI_TEST_DAEMON=required` is set. CI sets it on its runners, where Docker runs.
`cargo test` in this directory runs both.
