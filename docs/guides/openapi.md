# OpenAPI

`Api::openapi()` gives the OpenAPI 3.1 document of a contract, whatever the features of `quoll`. TypeScript generators
such as orval read it.

## The document

```rust
use quoll::rest::{self, Created};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
}

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    /// The signed-in user's notes.
    #[auth(Bearer)]
    pub group Notes {
        /// A note, by its id.
        ///
        /// Only the notes of the caller are found.
        GET  "/notes/{id}" => get(id: i64) -> Note;
        POST "/notes"      => create(note: NewNote) -> Created<Note>;
    }
}

let document = Api::openapi()
    .with_title("Notes")
    .with_version("1.2.0");
let json = document.to_json_pretty();

assert!(json.contains(r#""operationId": "notes_get""#));
assert!(json.contains(r#""summary": "A note, by its id.""#));
```

`Api::openapi()` returns a `rest::openapi::Document`:

- `to_json_pretty()` and `to_json()` write it, and it implements `Serialize`;
- `with_title(..)` and `with_version(..)` replace its `info`, which is the contract crate's name and version
  otherwise, from Cargo.

## A binary that prints it

The schemas are known only once the types are compiled, which a macro does not see. So the contract crate has a
binary that prints the document, `src/bin/openapi.rs`:

```rust
fn main() {
    println!("{}", notes_api::Api::openapi().to_json_pretty());
}
```

```bash
cargo run -p notes-api --bin openapi > openapi.json
```

The contract crate needs neither feature of `quoll`: it builds without axum and reqwest.

## `cargo quoll gen`

`cargo quoll`, a cargo subcommand, builds and runs that binary, writes the document, and runs the commands that follow
it, such as orval. It is installed from git until it is published:

```bash
cargo install --git https://github.com/4thlabs/quoll quoll-cli
```

Its config is in the workspace's `Cargo.toml`, with paths from the workspace's root:

```toml
[[workspace.metadata.quoll.api]]
package = "notes-api"
bin = "openapi"
output = "web/openapi.json"
after = [["npx", "--no", "orval"]]

[workspace.metadata.quoll.web]
dir = "web"
build = ["npm", "run", "build"]
```

- `cargo quoll gen` writes each api's `output` when its content changed, and leaves it as it is otherwise. Then it
  runs the api's `after` commands in order, in the directory of `output`, whether the document changed or not.
- `cargo quoll gen --check` writes nothing and runs nothing: it fails when a document on disk is not the one its
  contract prints, or is missing. It is the check of a project that commits its document. Cargo builds the contract
  with `--locked`, so that the check does not update the `Cargo.lock` either.
- `cargo quoll build` runs `gen`, then `web.build` in `web.dir`.

A document compares with its line endings read as LF, so that a checkout with `core.autocrlf` is not stale. A
committed document is best kept as the contract prints it, with LF, in `.gitattributes`:

```text
web/openapi.json text eol=lf
```

A command is a list of words, run with no shell: its program is found on the `PATH` as the OS does, so that `npm` is
`npm.cmd` on Windows. Each fails with the output of the cargo build or of the command that failed, and a config that
cannot be read names the key at fault, `workspace.metadata.quoll.api[0].output: missing`.

## `cargo quoll dev`

`cargo quoll dev` runs `gen`, starts the servers and the web side's dev server, then makes again what a change of a
file touches, until Ctrl-C. Its servers and its `web.dev` are named in the config too:

```toml
[[workspace.metadata.quoll.server]]
package = "notes-server"
url = "http://127.0.0.1:8080"

[workspace.metadata.quoll.web]
dir = "web"
dev = ["npm", "run", "dev"]
build = ["npm", "run", "build"]
```

- A server runs the binary `cargo run` would run: its package's only binary, or its `default-run`. It is ready when
  its `url` accepts a connection.
- A change to a contract, or to a crate it is built with, writes its document when its content changed, runs its
  `after` commands then, or until they succeeded once, and restarts the servers built with it. A document that did
  not change is left as it is, so that the bundler reloads nothing.
- A change to a server restarts that server alone. The one that runs stops only once the new one has compiled: a
  server that does not compile prints cargo's errors, and the last one that compiled keeps answering.
- `web.dev` starts once every server is ready or has failed, in `web.dir`, and runs until the end: the bundler
  watches its own files, the TypeScript that `after` writes among them.
- Ctrl-C, or `SIGTERM`, stops `web.dev`, then the servers, each with what it started, on Linux, macOS and Windows.

The workspace's `Cargo.toml` and `Cargo.lock` are watched too, and the path dependencies outside the workspace. A
package's own `target/`, hidden files, `node_modules`, the documents and `web.dir`, but the crates inside it, are not.
On Linux, a package at the workspace's root that holds the web side's `node_modules` may use up inotify's watches,
which a warning names: raise `fs.inotify.max_user_watches`, or keep the web side out of that package. `cargo quoll dev --json` prints one
JSON line per event on stdout, for a tool that reads them, and its output for a person on stderr:

```text
{"event":"document","package":"notes-api","path":"web/openapi.json","changed":true}
{"event":"server","state":"building","package":"notes-server","url":"http://127.0.0.1:8080"}
{"event":"error","source":"cargo","package":"notes-server","message":"mismatched types","file":"server/src/main.rs","line":12,"column":5}
{"event":"server","state":"failed","package":"notes-server","url":"http://127.0.0.1:8080"}
{"event":"server","state":"ready","package":"notes-server","url":"http://127.0.0.1:8080"}
{"event":"web","state":"started"}
```

The [dev loop of the spec](../spec.md#the-dev-loop) lists every event and its fields.

## The web side

`cargo quoll` knows no bundler and no generator: the web side follows conventions, as the notes example's page,
[`examples/notes/web`](../../examples/notes/web), does.

- The bundler's own proxy sends `/api` to the server's `url`, Vite's `server.proxy`: the page calls the api on its
  own origin. The bundler's config repeats the `url`: the two change together. It needs nothing else for `Events`:
  an `Events` answer opens with a comment, which Vite's proxy, holding the head of an answer until its first bytes,
  lets through at once, and a server that `cargo quoll dev` restarts ends its `Events` cleanly, so that the page sees
  its stream open and end. A `JsonLines` or a `ByteStream` is seen to open with its first line or chunk, and a
  restart cuts it, which Vite's proxy may not pass on: the page's side may stay open, a limit of the dev loop
  ([#70](https://github.com/4thlabs/quoll/issues/70)).
- orval's config reads the document, `input: "./openapi.json"`, and `after` runs it, `npx --no orval`: the orval
  that `npm ci` installs, at the version the lock file pins, or an error that asks for it. Its mutator, one function
  of the application, adds the credential and throws the problem of an error; with `httpClient: "fetch"`,
  `override.fetch.includeHttpResponseReturnType: false` makes a hook's data the body the mutator returns.
- `EventSource` cannot send a header credential: the page reads an `Events` route with `fetch` and
  `response.body.getReader()`, in a few lines that split the events.

```bash
npm ci --prefix examples/notes/web
cargo quoll dev
```

At the root of the repository, it runs the notes server, orval and Vite, on `http://localhost:5173`. The server
prints the demo's password when it starts.

## What it holds

- **Paths**: each route under the api's prefix, its operation named `group_function`, `notes_get`, and tagged with
  its group. The first paragraph of a route's `///` is its summary, the rest its description; a group's describes its
  tag, and the api's the document.
- **Parameters**: path parameters, query keys and headers, required unless an `Option` or a `Vec`. A query struct is
  unfolded into one parameter per field.
- **Bodies**: `application/json`, `application/x-www-form-urlencoded` for `Form<T>`, `text/plain` for `Text`,
  `application/octet-stream` for `Bytes`.
- **Results**: 200, 201 with an optional `Location`, 202 or 204. `Text` is `text/plain`, `Bytes` and `ByteStream` are
  `*/*`. An `Events` or a `JsonLines` result carries the schema of its items in the extension `x-item`.
- **Errors**: each route lists the statuses quoll answers itself, and a `default`, each an `application/problem+json`
  problem.
- **Security**: each scheme under `components/securitySchemes`, by its name, and each route's requirement, with its
  scopes, and `{}` when its credential is optional.
- **Schemas**: under `components/schemas`, from schemars.

The doc of `quoll::rest::openapi` says each of them in full.

## Schemas and their names

Each type a contract names implements `schemars::JsonSchema`. A schema is named as schemars names it:

- a second type named `Note`, in another module, is `Note2`, in the order the contract names them;
- `#[schemars(rename = "SharedNote")]` gives a type a name of its own;
- a type read and written otherwise, as one with a `#[serde(default)]` field, has two schemas, `NoteInput` and
  `NoteOutput`: a client may leave the field out, but always receives it.

```rust
use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// A note's settings: a client may leave out `pinned`, and always receives it.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Settings {
    #[serde(default)]
    pub pinned: bool,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        PUT "/notes/{id}/settings" => configure(id: i64, settings: Settings) -> Settings;
    }
}

let json = Api::openapi().to_json();

assert!(json.contains("SettingsInput"));
assert!(json.contains("SettingsOutput"));
```

## A contract that is only called

The contract of an API that quoll did not write, which the application only calls, says `#[openapi(none)]` on its
api. It has no `Api::openapi()`, and its types need serde alone: they may come from crates that do not implement
`JsonSchema`.

```rust
use quoll::rest;
use serde::{Deserialize, Serialize};

/// A repository, as GitHub's API answers it.
#[derive(Debug, Serialize, Deserialize)]
pub struct Repo {
    pub full_name: String,
}

rest::api! {
    /// GitHub's API, which the application only calls.
    #[openapi(none)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: String, repo: String) -> Repo;
    }
}
```

A `rest::group!` of such a contract says `#[openapi(none)]` on its group. An api with a document lists only groups
that have one.
