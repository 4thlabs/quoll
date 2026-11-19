<div align="center">
  <img src="docs/assets/quoll.svg" alt="" width="104" />

  <h1>Quoll</h1>

  <p><strong>A REST contract for Rust, written once: an axum server, a reqwest client and an OpenAPI document come
  from it.</strong><br />
  TypeScript generators such as orval turn the document into TanStack Query hooks.</p>

  <p>
    <img src="https://img.shields.io/badge/Rust-2024-B7410E?logo=rust&logoColor=white" alt="Rust 2024" />
    <img src="https://img.shields.io/badge/OpenAPI-3.1-6BA539?logo=openapiinitiative&logoColor=white" alt="OpenAPI 3.1" />
    <img src="https://img.shields.io/badge/status-design-lightgrey" alt="Status: design" />
    <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-BSD%203--Clause-blue" alt="BSD 3-Clause" /></a>
  </p>

  <p>
    <a href="docs/spec.md"><strong>The spec</strong></a> ·
    <a href="docs/spec.md#a-complete-example">A complete example</a> ·
    <a href="docs/spec.md#open-questions">Open questions</a>
  </p>
</div>

## One contract, both sides

An API is written once, in a macro. The server implements it, the client calls it, and a mistake on either side, a
route without its handler, a call with the wrong arguments, does not compile.

```rust
rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}
```

<table>
<tr>
<th>The server</th>
<th>The client</th>
</tr>
<tr>
<td>

```rust
impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        self.sessions
            .user(&token)
            .ok_or_else(|| Error::unauthorized("unknown or expired token"))
    }
}

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64) -> Result<Note> {
        self.store
            .note(user.id, id)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    // create, remove...
}

let router = Api::router(ApiServices { notes: app });
```

</td>
<td>

```rust
let http = ClientBuilder::new(reqwest::Client::new())
    .with(rest::client::bearer(token))
    .build();

let api = Api::client_with(http, "http://127.0.0.1:8080")?;

let note = api.notes().get(3).await?;

println!("{}", note.title);
```

</td>
</tr>
</table>

## What the contract gives

| From the contract | What it is                                                                           |
| ----------------- | ------------------------------------------------------------------------------------ |
| `NotesServer`     | The trait the server implements: a method per route, the caller as a typed argument |
| `Api::router`     | An `axum::Router`, which tower layers wrap: request id, trace, timeout, CORS...      |
| `Notes`           | The trait the client implements, and test fakes too                                  |
| `Api::client`     | A reqwest client, with reqwest-middleware for tokens, retries and signatures         |
| `Api::openapi`    | The OpenAPI 3.1 document, with the schemas of the types and the `///` comments       |

No Rust is generated on disk: the macro expands at compile time, and `cargo expand` shows what it writes.

## Status

Design: the contract is specified in [`docs/spec.md`](docs/spec.md), a draft. There is no code yet.

## License

BSD 3-Clause, see [`LICENSE.md`](LICENSE.md).
