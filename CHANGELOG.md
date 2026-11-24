# Changelog

The notable changes of each version of quoll. The format follows [Keep a Changelog](https://keepachangelog.com/).

quoll follows [semantic versioning](https://semver.org/). While its major version is 0:

- a minor version, 0.9.0 to 0.10.0, may break the API;
- a patch version, 0.9.0 to 0.9.1, never does;
- `rust-version`, the oldest Rust that builds quoll, is raised only in a minor version.

quoll is used from git, at a version's tag:

```toml
[dependencies]
quoll = { git = "https://github.com/4thlabs/quoll", tag = "v0.9.1" }
```

## [Unreleased]

### Added

- A `&` on an argument's type makes the client borrow it, so that a call needs no `.into()`, and a body or a list no
  `.clone()`: `list(owner: &str, #[join(",")] labels: &[&str], state: Option<&str>)` is called as
  `list("rust-lang", &["C-bug"], Some("open"))`, and `create(note: &NewNote)` as `create(&note)`. The server and the
  OpenAPI document receive the owned type, `String`, `Vec<String>`, `Option<String>`, `NewNote`. An argument with no
  `&` is taken by value, as before. `&Form<T>`, `&Text`, `&Bytes`, `&[String]` and `query filter: &NoteFilter` are
  borrowed too. With the server, `&mut X`, `&&X`, `&dyn Trait`, `&impl Trait` and a `&X` whose `X` has no size, as
  `&Path`, do not compile; a contract with a client alone takes them as before. The docs and the examples write
  `&str` for a string of the path, the query or a header ([#119](https://github.com/4thlabs/quoll/issues/119)).

## [0.9.1] - 2026-11-24

### Added

- `ByteStream::with_length` sends a stream of a known size, such as a file, with its `Content-Length` rather than in
  chunked encoding. A stream that gives another number of bytes is cut before its last bytes and logged, and the
  client reads an error. `ByteStream::length` gives the length on both sides, and on the client the `Content-Length`
  of the answer ([#115](https://github.com/4thlabs/quoll/issues/115)).

### Changed

- `server::builder()`'s `serve` and `serve_on`, and `test::spawn`, set `TCP_NODELAY` on every connection whose stream
  is tokio's `TcpStream`, so that a small write, an event of a stream or a chunk of a `ByteStream`, is no longer held
  for about 40 ms by Nagle's algorithm ([#114](https://github.com/4thlabs/quoll/issues/114)).

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

[Unreleased]: https://github.com/4thlabs/quoll/compare/v0.9.1...HEAD
[0.9.1]: https://github.com/4thlabs/quoll/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/4thlabs/quoll/releases/tag/v0.9.0
