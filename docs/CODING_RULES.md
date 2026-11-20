# Quoll coding rules

Quoll is a Rust library, edition 2024, built with the latest stable Rust. The rules follow the
[Rust API Guidelines](https://rust-lang.github.io/api-guidelines/) and the
[style guide](https://doc.rust-lang.org/style-guide/). Where they say it already, this file does not repeat them.

## Tooling

- `cargo fmt` with the default configuration. There is no `rustfmt.toml`: the code looks like every other Rust crate.
- `cargo clippy --all-targets --all-features -- -D warnings` passes. A lint is allowed only on the item that needs it,
  with `#[expect(clippy::..., reason = "...")]`, never for the whole crate.
- The workspace's lints in `Cargo.toml` hold `unsafe_code = "forbid"` and `missing_docs = "warn"`. Each crate
  inherits them with `[lints] workspace = true`.
- The examples set their own lints, `unsafe_code = "forbid"` without `missing_docs`, so that they show only the
  comments a reader needs.
- `rust-version` in `Cargo.toml` states the minimum Rust, set at the first release and raised only in a minor version.

## Crates and modules

- One subject per module, in `name.rs`, with its submodules in `name/`. There is no `mod.rs`.
- `lib.rs` holds the crate's docs, its modules and what it re-exports; the code lives in the modules. A proc-macro
  crate keeps its `#[proc_macro]` entry points there too, as Rust requires. `main.rs` holds `main`, which calls the
  rest.
- What users name is re-exported where they look for it: `quoll::rest::Error`, not `quoll::rest::error::Error`.
  Everything else is `pub(crate)` or private.
- Features are additive and off by default: `server` adds axum, `client` adds reqwest. Code behind a feature carries
  `#[cfg(feature = "...")]` on the module rather than on each item.
- The dependencies the spec names are part of the API: axum's `Router`, reqwest-middleware's `ClientWithMiddleware`,
  http's `StatusCode`, `Method` and `HeaderMap`, which axum and reqwest both use, serde and schemars. Any other
  dependency stays out of public signatures, so that it can be replaced.
- No `unsafe`. A need for it goes to the maintainer first.

## The macro

- The generated code names everything by an absolute path, `::quoll::...`, and never relies on what the user's file
  imports.
- What only the generated code calls lives in `quoll::__private`, `#[doc(hidden)]`. It is not part of the API and may
  change in any version.
- The generated code stays thin: it builds a description and calls the crate, compiled once. Logic goes into `quoll`,
  not into the tokens.
- A mistake in a contract is a `syn::Error` spanned on the token at fault, with a message saying what to write instead.
  A proc macro never panics.

## API

- Names follow the guidelines: `UpperCamelCase` types and traits, `snake_case` functions and modules, `SCREAMING_SNAKE`
  constants. Conversions are `as_`, `to_` and `into_`; getters take no `get_`; constructors are `new` or `with_...`.
- Public types derive what they can: `Debug` always, then `Clone`, `PartialEq`, `Eq`, `Hash` and `Default` where they
  mean something. They are `Send` and `Sync` where they can be, and the futures the API returns are `Send`.
- What outlives a call owns what it needs. A public type borrows only when it is a view, and a user never has to keep a
  value alive for the library.
- A function takes what it reads as `&str` or `&[T]`, and what it keeps as `impl Into<String>` or by value.
- Async functions in traits are native, written `fn ... -> impl Future<Output = ...> + Send` where they must be `Send`.
  The `async-trait` crate is not used.
- Nothing blocks in async code. Work that blocks goes through `tokio::task::spawn_blocking`.

## Errors

- A failure a caller can cause is a `Result`, never a panic. `unwrap` and `expect` are for tests, examples and
  invariants the code itself guarantees, with an `expect` message saying which.
- Errors implement `std::error::Error` and `Display`. Their message is lowercase, with no final period, and keeps its
  `source()`.
- `?` carries an error up. `map_err` adds what the caller needs to know, and an error is never silently dropped.

## Readability

- Short functions, early returns, `let ... else` rather than nesting.
- A blank line between the steps of a function: after its bindings, and around a block that has statements before or
  after it.
- Descriptive names, except the conventional ones (`T`, `E`, `i` in a loop, `cx` for a context).
- No commented-out code, no `todo!()` and no empty stubs in what is committed.
- Comments say why, not what.

## Docs

- Every public item of the crates has a `///` comment, and each module a `//!` comment that says what it holds; an
  example has one where it teaches something.
- A public function's example is a doctest that compiles and runs, or is `no_run` when it needs a network.
- `# Errors` says when a function returns an error, `# Panics` when it may panic.
- No file has a license header: `license = "MIT"` in `Cargo.toml` says it.

## Tests

- Unit tests sit beside the code, in `#[cfg(test)] mod tests`. What goes through the public API is under `tests/`.
- A test runs what it claims. A server is served on port 0 and called by the real client. The test awaits what it
  checks, and never uses a sleep to wait for it.
- What must not compile is tested with trybuild: one file per mistake under `tests/compile_fail/`, with its `.stderr`
  beside it. A changed message is reviewed in the diff.
- A fixed bug gets the test that fails without its fix.
