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

Design only: there is no code yet. The spec is **Validated** by the maintainer: it changes only by a decision of theirs,
made in the pull request that updates it, with its new date.

## Build and test

```bash
cargo build --workspace
cargo test --workspace --all-features
cargo fmt --all                                                    # before every commit
cargo clippy --workspace --all-targets --all-features -- -D warnings
cargo doc --workspace --no-deps --all-features                     # the docs, with their links checked
cargo expand -p <crate> --test <test>                              # what the macro writes (cargo install cargo-expand)
TRYBUILD=overwrite cargo test -p quoll-macros --test compile_fail   # to accept a new or changed error message
```

## Layout

Planned, from the spec:

- `quoll/`: the crate users depend on. Its features `server` (axum) and `client` (reqwest) are each off by default.
- `quoll-macros/`: the proc macro behind `quoll::rest::api!`, re-exported by `quoll`, which users never name.
- `examples/notes/`: the spec's complete example, split into the contract, its server, its command line and its page.
  It is built and tested with the workspace.
- `docs/`: `spec.md` and the coding rules. Guides for users come with the code they describe.

What the macro generates is never written to disk: `cargo expand` shows it. The TypeScript that orval writes is
generated in the example, and gitignored.

## Repository

Hosted on GitHub: [`4thlabs/quoll`](https://github.com/4thlabs/quoll), MIT. CI and the protection of `main` come with
the first code, and their rules will be written here then.

## Related projects

- [quoll-http](https://github.com/4thlabs/quoll-http), archived (local checkout `../quoll-http`): the C++ framework
  this started from. Its `quoll::rest` is a source of ideas, not of code to port.
