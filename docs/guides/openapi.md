# OpenAPI

`Api::openapi()` gives the OpenAPI 3.1 document of a contract, whatever the features of `quoll`. Any OpenAPI generator
reads it to write the client of a frontend: orval, openapi-typescript, hey-api, Kiota...

## The document

```rust
use quoll::Created;
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

quoll::api! {
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

`Api::openapi()` returns a `quoll::openapi::Document`:

- `to_json_pretty()` and `to_json()` write it, and it implements `Serialize`;
- `with_title(..)` and `with_version(..)` replace its `info`. Otherwise the `info` is the contract crate's name and
  version, from Cargo.

## A binary that prints it

The schemas are known only once the types are compiled, and a macro does not see compiled types. So the contract
crate has a binary that prints the document, `src/bin/openapi.rs`:

```rust
fn main() {
    println!("{}", notes_api::Api::openapi().to_json_pretty());
}
```

In the directory of the notes example, `examples/notes`:

```bash
cargo run -p notes-api --bin openapi > openapi.json
```

The contract crate needs neither feature of `quoll`: it builds without axum and reqwest.

`cargo quoll gen` builds and runs that binary, and writes the document where the workspace's `Cargo.toml` says. Then
it runs the generator that the workspace names. [The TypeScript guide](typescript.md) says how. It also covers
`cargo quoll dev`, which runs the server and the page's dev server side by side.

## What it holds

- **Paths**: each route under the api's prefix. Its operation is named `group_function`, such as `notes_get`, and
  tagged with its group. The first paragraph of a route's `///` comment is its summary, and the rest is its
  description. A group's comment describes its tag, and the api's comment describes the document.
- **Parameters**: path parameters, query keys and headers, required unless an `Option` or a `Vec`. A query struct is
  unfolded into one parameter per field. A [joined list](contract.md#joined-lists) is a parameter with
  `explode: false`, in the style of its separator: `form`, `spaceDelimited` or `pipeDelimited`. A fixed header,
  `#[header("name" = value)]`, is a header parameter that is not required. Its schema is its value, as a `const`.
  There are two exceptions. `Accept`, `Content-Type` and `Authorization` are not, because OpenAPI describes them in
  other ways. Neither is a header that an argument of the route reads, because the argument describes it.
- **Bodies**: `application/json`, `application/x-www-form-urlencoded` for `Form<T>`, `text/plain` for `Text`,
  `application/octet-stream` for `Bytes`.
- **Results**: 200, 201 with an optional `Location`, 202 or 204. A 204 comes beside them when the handler's value
  may be empty: an `Option<T>`, `()`, `Text`, `Bytes`, `Events`, `JsonLines` or `ByteStream`, in a `Created` or an
  `Accepted` too. `Text` is `text/plain`, and `Bytes` and `ByteStream` are `*/*`. An `Events` or a `JsonLines` result
  carries the schema of its items in the extension `x-item`.
- **Errors**: each route lists the statuses that quoll answers itself, and a `default`. Each one is an
  `application/problem+json` problem.
- **Security**: each scheme under `components/securitySchemes`, by its name. Each route has its requirement, with its
  scopes, and `{}` when its credential is optional.
- **Schemas**: under `components/schemas`, from schemars. schemars sometimes writes a `$ref` beside other keywords, for
  example for a variant of a tagged enum that holds a struct. The document makes it an `allOf` of the reference and of
  those keywords, which the generators that read a `$ref` alone keep.

The doc of `quoll::openapi` says each of them in full.

## Schemas and their names

Each type that a contract names implements `schemars::JsonSchema`. A schema gets the name that schemars gives it:

- a second type named `Note`, in another module, is `Note2`, in the order the contract names them;
- `#[schemars(rename = "SharedNote")]` gives a type a name of its own;
- a type that is read and written in different ways, such as one with a `#[serde(default)]` field, has two schemas,
  `NoteInput` and `NoteOutput`: a client may leave the field out, but always receives it.

```rust
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// A note's settings: a client may leave out `pinned`, and always receives it.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Settings {
    #[serde(default)]
    pub pinned: bool,
}

quoll::api! {
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

The contract of an API that quoll did not write, and that the application only calls, says `#[openapi(none)]` on its
api. It has no `Api::openapi()`, and its types need only serde: they may come from crates that do not implement
`JsonSchema`.

```rust
use serde::{Deserialize, Serialize};

/// A repository, as GitHub's API answers it.
#[derive(Debug, Serialize, Deserialize)]
pub struct Repo {
    pub full_name: String,
}

quoll::api! {
    /// GitHub's API, which the application only calls.
    #[openapi(none)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: &str, repo: &str) -> Repo;
    }
}
```

A `quoll::group!` of such a contract says `#[openapi(none)]` on its group. An api with a document lists only groups
that have one.

[Another API](another-api.md) goes through such a contract:

- its headers;
- its errors;
- its forms;
- its pages.
