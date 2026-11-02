# AGENTS.md

Guidance for AI coding agents working in this repository.

**Before writing or reviewing code, read [`docs/CODING_RULES.md`](docs/CODING_RULES.md)** — the C++ rules every
change must follow. Target architecture, deployment modes and open decisions are in [`docs/PLAN.md`](docs/PLAN.md):
read it first, it is the source of truth for the design.

Quoll is a public, header-only C++20 framework for cross-platform applications: sans-IO async runtime, HTTP client and
server, and a typed REST DSL exposed to a web UI through tRPC. The same application runs as a native server + web app,
a standalone PWA (core in WASM), or a native webview shell. Namespace `quoll::`.

The maintainer communicates in French; code, comments, docs and commit messages stay in English.

## Status

Exploration: no code yet. The next step is the prototype described in `docs/PLAN.md`; it decides the HTTP layer
(own stack + llhttp, the current leaning, vs Boost.Capy + Boost.Http). Do not settle open decisions or write the
architecture into this file before the prototype validates them.

## Hard constraints

- Header-only, C++20 minimum (GCC ≥ 12, Clang ≥ 17). CMake, dependencies via CPM.
- WASM is a first-class target: core modules never require threads or blocking waits.
- No Qt. Adapters planned: thread pool, asio, Emscripten (browser loop, `fetch`, `postMessage`), webview shell, memory
  (tests).

## Related projects

- [Puffin](https://github.com/4thlabs/puffin) (local checkout `../puffin`): the maintainer's utility library
  (events, ioc, maths, imdb) where this plan started. Its `claude/project-thread-*` branches hold an earlier
  `async` / `webkit` / `rest` attempt, removed for quality reasons: ideas only, never port code from them.
- `cpp-init` (`../cpp-init`): the maintainer's project scaffolding tool (`npx @4thlabs/cpp-init`).

## Repository

Hosted on GitHub: [`4thlabs/quoll`](https://github.com/4thlabs/quoll). No CI yet. Formatting: `clang-format -i <file>`
with the repo `.clang-format`; format only the files you touch.
