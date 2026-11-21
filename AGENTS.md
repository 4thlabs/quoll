# AGENTS.md

Guidance for AI coding agents working in this repository.

**Before writing or reviewing code, read [`docs/CODING_RULES.md`](docs/CODING_RULES.md)**: the Rust rules every change
follows. **The design is in [`docs/spec.md`](docs/spec.md)**: read it first, it is the source of truth for what the
crate does. **What is to do, and in which order, is in the GitHub project
[Quoll](https://github.com/users/4thlabs/projects/2)**: one issue per piece of work, grouped by milestone.

Quoll is a REST contract for Rust, written once in a macro: an axum server, a reqwest client and an OpenAPI 3.1
document come from it, and TypeScript generators such as orval turn the document into TanStack Query hooks. There is no
UI. The crate is `quoll`, the contract its module `quoll::rest`.

The maintainer writes in French. Code, comments, docs and commit messages are in English.

## Status

Design first: the workspace and its CI stand, and the features of the spec come issue by issue. The spec is
**Validated** by the maintainer: it changes only by a decision of theirs, made in the pull request that updates it,
with its new date.

## Build and test

At the root, the library's workspace:

```bash
cargo build --workspace
cargo test --workspace --all-features
cargo fmt --all                                                    # before every commit
cargo clippy --workspace --all-targets --all-features -- -D warnings
cargo doc --workspace --no-deps --all-features                     # the docs, with their links checked
cargo expand -p <crate> --test <test>                              # what the macro writes (cargo install cargo-expand)
TRYBUILD=overwrite cargo test -p quoll --test compile_fail         # to accept a new or changed error message
cargo test -p quoll-guides                                         # the Rust blocks of the guides and the README
cargo install --path quoll-cli                                     # `cargo quoll`
```

Each example is a workspace of its own, with its `Cargo.lock`, built and tested from its directory, `examples/notes`
or `examples/docker`, the same `cargo fmt`, `clippy`, `test` and `doc` as at the root, without `--all-features`. In
`examples/notes`:

```bash
SNAPSHOT=overwrite cargo test -p notes-api --test openapi          # to accept a changed OpenAPI document of the example
npm ci --prefix web                                                # the page's dependencies, on Node 22
cargo quoll build                                                  # the notes document, orval, then the page's build
npm test --prefix web                                              # the page's tests, once orval has written its hooks
cargo quoll dev                                                    # the notes server, orval and Vite, until Ctrl-C
```

## Layout

Planned, from the spec:

- `quoll/`: the crate users depend on. Its features `server` (axum) and `client` (reqwest) are each off by default.
- `quoll-macros/`: the proc macro behind `quoll::rest::api!`, re-exported by `quoll`, which users never name.
- `quoll-cli/`: the binary `cargo-quoll`, `cargo quoll gen`, `build` and `dev`, configured in a workspace's
  `[workspace.metadata.quoll]`, as the notes example's is. It does not depend on `quoll`: it runs the contract's
  binary. Its tests run it on a small workspace under `tests/fixture/`, copied for each test, whose server and
  `web.dev` listen on ports.
- `examples/`: each example a workspace of its own, as an application's is, which depends on `quoll` by path, has its
  own lints and its own `Cargo.lock`, and is left out of the root's, `exclude`d.
- `examples/notes/`: the spec's complete example, split into the contract, its server, its command line and its page,
  with the config of `cargo quoll` in its `Cargo.toml`. `web/` is the page, Vite, React and the TanStack Query hooks
  orval writes from `openapi.json`, which `cargo quoll gen` writes there, both gitignored; its npm dependencies are
  pinned, with their `package-lock.json`, on Node 22. `cargo quoll dev` in `examples/notes` runs the server, orval and
  Vite; `npm test` in `web/` runs its vitest tests, once `cargo quoll gen` has written the TypeScript.
- `examples/docker/`: a contract of the Docker Engine API, which it only calls, and a command line on it; its test
  against a real daemon is skipped where none answers, but in CI.
- `docs/`: `spec.md`, the coding rules, and the user guides, indexed by `docs/README.md`. `docs/guides/` is the crate
  `quoll-guides` too, whose doctests compile and run the Rust blocks of each guide and of the README: a block that
  needs a network is `rust,no_run`, one of tests `rust,test_harness`, one that shows a mistake `rust,compile_fail`.
  Its dev-dependency on `notes-api` is a path into the notes example's workspace, which cargo builds from the root.
  A guide says how to use quoll and links to the rustdoc for each item's detail; the why stays in the spec.

What the macro generates is never written to disk: `cargo expand` shows it. The TypeScript that orval writes is
generated in the example, and gitignored.

## Repository

Hosted on GitHub: [`4thlabs/quoll`](https://github.com/4thlabs/quoll), MIT.

- Every change goes through a pull request: `main` is protected, with no direct push.
- CI (`.github/workflows/ci.yml`) runs on every pull request and every push to `main`: `cargo fmt --check`,
  `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace` and
  `cargo doc --workspace --no-deps`, the last three once per feature combination of `quoll`: none, `server`, `client`
  and both. Those leave the guides out, `--exclude quoll-guides`: they turn `quoll`'s features on, and cargo would
  unify them into the whole build, so a local `cargo test --workspace` builds `quoll` with both. The guides have their
  own run of the three, `--package quoll-guides`, and so has `quoll-cli`, `--package quoll-cli`, which does not depend
  on `quoll`; its clippy and tests run on macOS and Windows too, in the job `cli`, where `cargo quoll dev` stops its
  children in other ways. The job `examples` runs the four in each example's directory, with the features its crates
  choose and `--locked`, so that its committed `Cargo.lock` is the one it builds with; the Docker example's tests set
  `DOCKER_CLI_TEST_DAEMON=required`, so that its test fails when the runner's daemon does not answer. The job `web`
  builds `cargo-quoll` at the root and runs `cargo quoll build` in `examples/notes`, which writes the notes document,
  runs orval and bundles the page, then the page's `npm test`: a change of the contract that breaks the page breaks
  the pull request. The document itself is checked by the snapshot test of the
  notes contract, `notes-api`'s `tests/openapi.rs`. Warnings are errors, in rustc and in rustdoc.
- The `ci` job gathers all of them and is the one check `main` requires: a new job goes in its `needs`. The branch must
  be up to date with `main`, and every review thread resolved, before the merge.
- A pull request is merged with a rebase, and its branch deleted. Closing its issue is done in its description with
  `Closes #N`.
- There is no Codex review workflow in this repository.

## Related projects

- [quoll-http](https://github.com/4thlabs/quoll-http), archived (local checkout `../quoll-http`): the C++ framework
  this started from. Its `quoll::rest` is a source of ideas, not of code to port.
