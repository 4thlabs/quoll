# AGENTS.md

Guidance for AI coding agents working in this repository.

**Before writing or reviewing code, read [`docs/CODING_RULES.md`](docs/CODING_RULES.md)**: the Rust rules every change
follows. **The design is in [`docs/spec.md`](docs/spec.md)**: read it first. It is the source of truth for what the
crate does. **The work to do, and its order, is in the GitHub project
[Quoll](https://github.com/users/4thlabs/projects/2)**: one issue per piece of work, grouped by milestone.

**A modern, type-safe REST toolkit for Rust: from a client for any API to the server behind your frontend.** One
contract gives you an axum server, a reqwest client and an OpenAPI 3.1 document. `cargo quoll` integrates it with your
frontend and keeps both in sync as you work. Any OpenAPI generator works: the notes example uses orval,
which writes TanStack Query hooks. There is no UI. The crate is `quoll`, with the contract at its root.

The maintainer writes in French. Code, comments, docs and commit messages are in English.

## Status

The milestones of the spec are done:

- the contract, the server and the client;
- the OpenAPI document;
- `cargo quoll gen`, `build` and `dev`;
- the notes page and the guides.

quoll is used from git and has no version yet. The first release is [#21](https://github.com/4thlabs/quoll/issues/21).
What comes next is in the milestone [Later](https://github.com/4thlabs/quoll/milestone/5), one issue per piece of work.
The spec is **Validated** by the maintainer. It changes only by their decision, made in the pull request that updates
it, with its new date.

## Build and test

At the root, in the library's workspace:

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

Each example is its own workspace, with its own `Cargo.lock`. Build and test it from its directory, `examples/notes`
or `examples/docker`, with the same `cargo fmt`, `clippy`, `test` and `doc` as at the root, but without
`--all-features`. In `examples/notes`:

```bash
SNAPSHOT=overwrite cargo test -p notes-api --test openapi          # to accept a changed OpenAPI document of the example
npm ci --prefix web                                                # the page's dependencies, on Node 22
cargo quoll build                                                  # the notes document, orval, then the page's build
npm test --prefix web                                              # the page's tests, once orval has written its hooks
cargo quoll dev                                                    # the notes server, orval and Vite, until Ctrl-C
```

## Layout

- `quoll/`: the crate users depend on. Its features `server` (axum) and `client` (reqwest) are each off by default.
- `quoll-macros/`: the proc macro behind `quoll::api!`. `quoll` re-exports it, and users never name it.
- `quoll-cli/`: the binary `cargo-quoll`, which runs `cargo quoll gen`, `build` and `dev`. A workspace configures it in
  its `[workspace.metadata.quoll]`, like the notes example does. It does not depend on `quoll`: it runs the contract's
  binary. Its tests run it on a small workspace under `tests/fixture/`, copied for each test. The fixture's server and
  its `web.dev` listen on ports.
- `examples/`: each example is its own workspace, like an application. It depends on `quoll` by path, has its own
  lints and its own `Cargo.lock`, and is excluded from the root workspace.
- `examples/notes/`: the spec's complete example. It is split into the contract, its server, its command line and its
  page, and the config of `cargo quoll` is in its `Cargo.toml`. `web/` is the page: Vite, React and the TanStack Query
  hooks. orval writes the hooks from `openapi.json`, which `cargo quoll gen` writes there; both are gitignored. The
  page's npm dependencies are pinned, with a `package-lock.json`, on Node 22. In `examples/notes`, `cargo quoll dev`
  runs the server, orval and Vite. In `web/`, `npm test` runs the vitest tests, once `cargo quoll gen` has written
  the TypeScript.
- `examples/docker/`: a contract of the Docker Engine API, which the example only calls, and a command line that uses
  it. Its test against a real daemon is skipped when no daemon answers, except in CI.
- `docs/`: `spec.md`, the coding rules, and the user guides, indexed by `docs/README.md`.

  `docs/guides/` is also the crate `quoll-guides`. Its doctests compile and run the Rust blocks of each guide and of
  the README:

  - a block that needs a network is `rust,no_run`;
  - a block of tests is `rust,test_harness`;
  - a block that shows a mistake is `rust,compile_fail`.

  Its dev-dependency on `notes-api` is a path into the notes example's workspace, which cargo builds from the root.

  GitHub shows the lines that rustdoc hides, so the README's short blocks get their hidden lines from `build.rs`. A
  block after `<!-- doctest NAME: WORD... -->` begins, hidden, with each file of `preludes/` and each earlier block
  that the comment names. Its doctests run on a copy in `OUT_DIR` that keeps the README's line numbers, and each one
  is named by the line of its comment.

  A guide says how to use quoll and links to the rustdoc for the details of each item. The why stays in the spec.

  The TypeScript and TOML blocks of the TypeScript guide are excerpts of the notes example. Each one is under the name
  of its file, and the page's `src/guide/excerpts.test.ts` checks them. The code that the page does not use is in
  `src/guide/`. The blocks of other apps, in the sections Next.js and Another generator, are checked by hand.

What the macro generates is never written to disk: `cargo expand` shows it. The TypeScript that orval writes is
generated in the example, and gitignored.

## Repository

Hosted on GitHub: [`4thlabs/quoll`](https://github.com/4thlabs/quoll), MIT.

- Every change goes through a pull request: `main` is protected, with no direct push.
- CI (`.github/workflows/ci.yml`) runs on every pull request and every push to `main`. Warnings are errors, in rustc
  and in rustdoc. It runs:

  - `cargo fmt --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace` and
    `cargo doc --workspace --no-deps`. The last three run once per feature combination of `quoll`: none, `server`,
    `client` and both. They leave the guides out, with `--exclude quoll-guides`. The guides turn on `quoll`'s
    features, and cargo would unify them into the whole build. So a local `cargo test --workspace` builds `quoll`
    with both features.
  - The same three for the guides, with `--package quoll-guides`.
  - The same three for `quoll-cli`, with `--package quoll-cli`, which does not depend on `quoll`. Its clippy and
    tests also run on macOS and Windows, in the job `cli`, where `cargo quoll dev` stops its children in other ways.
  - The job `examples`, which runs the four in each example's directory. It uses the features that the example's
    crates choose, and `--locked`, so that it builds with the committed `Cargo.lock`. The Docker example's tests set
    `DOCKER_CLI_TEST_DAEMON=required`, so that its test fails when the runner's daemon does not answer.
  - The job `web`, which builds `cargo-quoll` at the root and runs `cargo quoll build` in `examples/notes`. That
    writes the notes document, runs orval and bundles the page. Then the job runs the page's `npm test`. A change of
    the contract that breaks the page breaks the pull request.

  The snapshot test of the notes contract checks the document itself: `tests/openapi.rs` in `notes-api`.
- The `ci` job gathers all the others and is the one check that `main` requires: a new job goes in its `needs`. Before
  the merge, the branch must be up to date with `main`, and every review thread resolved.
- A pull request is merged with a rebase, and its branch deleted. Its description closes its issue with `Closes #N`.
- There is no Codex review workflow in this repository.

## Related projects

- [quoll-http](https://github.com/4thlabs/quoll-http), archived (local checkout `../quoll-http`): the C++ framework
  this started from. Its `quoll::rest` is a source of ideas, not of code to port.
