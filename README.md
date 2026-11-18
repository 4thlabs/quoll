# Quoll

A REST contract for Rust, written once: from it come an axum server, a reqwest client and an OpenAPI document, which
TypeScript generators such as orval turn into TanStack Query hooks.

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

## Status

Design: the contract is specified in [`docs/spec.md`](docs/spec.md), a draft. There is no code yet.

Quoll started as a C++ web framework, now archived as [quoll-http](https://github.com/4thlabs/quoll-http).

## License

BSD 3-Clause, see [`LICENSE.md`](LICENSE.md).
