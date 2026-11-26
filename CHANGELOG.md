# Changelog

The notable changes of each version of quoll. The format follows [Keep a Changelog](https://keepachangelog.com/).

quoll follows [semantic versioning](https://semver.org/). While its major version is 0:

- a minor version, 0.9.0 to 0.10.0, may break the API;
- a patch version, 0.9.0 to 0.9.1, never does;
- `rust-version`, the oldest Rust that builds quoll, is raised only in a minor version.

quoll is used from git, at a version's tag:

```toml
[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll", tag = "v0.11.0" }
```

## [Unreleased]

### Changed

- **Schemes by side**: `Scheme` keeps `NAME`, `ServerScheme` reads and `ClientScheme` writes; `client::key::<S>` sends
  any scheme, and `quoll::auth` reads `Authorization` ([#151](https://github.com/4thlabs/quoll/issues/151)).

Migration: keep `NAME` in `impl Scheme` and move `Credential`, `CHALLENGE`, `read` and `openapi` to
`impl ServerScheme`. An api or a `quoll::group!` with both sides also needs `impl ClientScheme`, which replaces the
`on_request` that sent the credential, or `#[client(none)]`. A declared scheme has only the sides of its api: a
`quoll::group!` with both sides that takes one of a `#[server(none)]` api writes `#[server(none)]`, one of a
`#[client(none)]` api writes `#[client(none)]`, and `client::key` takes no scheme of a `#[client(none)]` api.

## [0.11.0] - 2026-11-26

### Added

- **`cargo quoll dev`**: `env` and `args` for a server, and `env` for `web.dev`, in the workspace's config
  ([#138](https://github.com/4thlabs/quoll/issues/138)).
- **Error headers**: `Error::with_header` and `Error::retry_after`, sent with the error and read back by the client;
  `server::cors` exposes `Retry-After` and `Allow` ([#139](https://github.com/4thlabs/quoll/issues/139)).
- **Problem type and members**: `Error::with_type` and `Error::with_member`, the RFC 9457 `type` and extension members
  of a handler's problem ([#140](https://github.com/4thlabs/quoll/issues/140)).

### Changed

- An error's `Debug` names its headers and members, never their values.

### Fixed

- `.page(dir)` redirects only a directory that holds an `index.html`: Vite's `assets/` no longer loops with a
  trailing-slash rule ([#137](https://github.com/4thlabs/quoll/issues/137)).

## [0.10.0] - 2026-11-25

### Added

- **Error kinds**: `error.kind()`, a `quoll::ErrorKind` that tells a status from a refused connection, a timeout, a body
  cut short or an answer that does not read ([#126](https://github.com/4thlabs/quoll/issues/126)). `Error::failed`
  builds one for a fake, and `client::Attempt::kind()` gives it to a rule of `retry`.
- **`client::follow`**: follows a stream route again when it ends or fails, with a backoff, as `Followed` states
  ([#127](https://github.com/4thlabs/quoll/issues/127)).
- **One-sided contracts**: `#[server(none)]` and `#[client(none)]` leave out a side whatever `quoll`'s features
  ([#129](https://github.com/4thlabs/quoll/issues/129)).
- **`TcpTunnel`**: a route `-> quoll::TcpTunnel` opens a raw tunnel on its connection, as Docker's `exec`
  ([#124](https://github.com/4thlabs/quoll/issues/124)).
- **`send_timeout`**: the server cuts a connection whose client stops reading, after 60 s by default
  ([#128](https://github.com/4thlabs/quoll/issues/128)).

### Changed

- `serve`, `serve_on` and `test::spawn` cut a stalled client after 60 s; `.without_send_timeout()` keeps the old
  behaviour. quoll needs tokio 1.50 or later.
- `client::trace` records `error.type` on its span.
- An `event: problem` whose data is not a problem is skipped as a `Decode` item, and the stream goes on.
- `client::retry` tries an idempotent request again after any `Closed` failure.
- rustc's messages name quoll's `Status` type `quoll::Status`.

### Fixed

- A contract built with neither feature no longer leaves the import of an argument's type unused
  ([#129](https://github.com/4thlabs/quoll/issues/129)).

## [0.9.2] - 2026-11-24

### Added

- **Borrowed arguments**: a `&` on an argument's type makes the client borrow it, `create(&note)` rather than
  `create(note.clone())`; the server still receives the owned type
  ([#119](https://github.com/4thlabs/quoll/issues/119)).

## [0.9.1] - 2026-11-24

### Added

- `ByteStream::with_length` sends a stream of a known size with its `Content-Length`
  ([#115](https://github.com/4thlabs/quoll/issues/115)).

### Changed

- The server sets `TCP_NODELAY` on every connection, so that a small write is no longer held back
  ([#114](https://github.com/4thlabs/quoll/issues/114)).

## [0.9.0] - 2026-11-23

The first release. One contract gives an axum server, a reqwest client and an OpenAPI 3.1 document. `cargo quoll`
keeps a frontend in sync with it. It needs Rust 1.91 or later.

### Added

- **The contract**: `quoll::api!`, a route table checked at compile time.
  - Methods, paths and queries, read by name: path and query parameters, query structs, headers and a body in JSON,
    `Form`, `Text` or `Bytes`.
  - Results with their status: a value, `Created`, `Accepted`, no content, and the streams `Events`, `JsonLines` and
    `ByteStream`.
  - Groups, each with its own trait, and large apis split across modules with `quoll::group!`.
  - Authentication schemes: `Bearer`, `Basic`, and keys in a header, a cookie or the query, with alternatives and
    optional credentials.
  - Fixed headers with `#[header]`, and fixed fields of a body with `#[body]`.
  - Errors as RFC 9457 problems, or in the shape of another API's errors with `#[error(E)]`.
  - A mistake is a compile error on the token at fault, with a message that says what to write instead.
- **The server**, behind the feature `server`.
  - One trait per group, served on an `axum::Router` by `server::builder()`, until Ctrl-C or `SIGTERM`.
  - `Authenticate<S>` turns a credential into the caller each handler receives.
  - Layers: `request_id`, `trace`, `cors`, `compression`, `timeout` and `body_limit`, put in the order they work in.
  - `quoll::test` serves a router on a port of its own, for a test. It comes with the feature `server`, and a test
    that calls the router with the real client also needs the feature `client`.
- **The client**, behind the feature `client`.
  - One method per route, built by `client::builder()` on reqwest-middleware.
  - Middleware: credentials, tokens that expire, `retry`, `timeout`, `redirects`, `trace`, `request_id`,
    `user_agent` and `on_request`.
  - A fake of each group, for the tests of code that calls an api.
  - Contracts of APIs that quoll did not write, such as the Docker Engine API, over TCP or a Unix socket.
- **The OpenAPI document**: `Api::openapi()` gives the OpenAPI 3.1 document of a contract, with the schemas of its
  types and its `///` comments.
- **`cargo quoll`**, the crate `quoll-cli`.
  - `cargo quoll gen` writes the document and runs the generator the workspace names: orval, openapi-typescript or
    any other. `gen --check` fails when a committed document is out of date.
  - `cargo quoll build` runs `gen`, then the page's build.
  - `cargo quoll dev` runs the servers, the generator and the page's dev server. When a file changes, it writes the
    document again and restarts the servers it touches.
  - Its config is in the workspace's `Cargo.toml`, under `[workspace.metadata.quoll]`.
- **Serving the page**: `server::builder().page(dir)` serves a built page beside the api, on one origin. A page's
  router gets `index.html` for its paths, and the files Vite names by their hash are cached for a year.

[Unreleased]: https://github.com/4thlabs/quoll/compare/v0.11.0...HEAD
[0.11.0]: https://github.com/4thlabs/quoll/compare/v0.10.0...v0.11.0
[0.10.0]: https://github.com/4thlabs/quoll/compare/v0.9.2...v0.10.0
[0.9.2]: https://github.com/4thlabs/quoll/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/4thlabs/quoll/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/4thlabs/quoll/releases/tag/v0.9.0
