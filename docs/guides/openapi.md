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
after = [["npx", "orval"]]

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
