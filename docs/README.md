# The guides of quoll

Each guide says how to use one part of quoll, with examples to copy. The API documentation, `cargo doc -p quoll
--all-features --open`, says what each item does; [the spec](spec.md) says why the design is what it is.

| Guide                                           | What it covers                                                                |
| ----------------------------------------------- | ----------------------------------------------------------------------------- |
| [Getting started](guides/getting-started.md)    | quoll from git, a first contract, its server and its client, and running them |
| [The contract](guides/contract.md)              | methods and paths, parameters, bodies, results, streams, groups across modules |
| [The server](guides/server.md)                  | services, `ApiServices`, the builder and its layers, serving and shutdown     |
| [The client](guides/client.md)                  | the builder, its middleware, credentials, retries and redirects, a Unix socket |
| [Authentication](guides/authentication.md)      | schemes, alternatives, optional credentials, scopes, cookies and their origin |
| [Errors](guides/errors.md)                      | `rest::Error`, the problems a server answers, what a client sees              |
| [Testing](guides/testing.md)                    | a server and its client in a test, fakes, the OpenAPI snapshot                |
| [OpenAPI](guides/openapi.md)                    | `Api::openapi()`, what the document holds, contracts that are only called     |

The Rust blocks of the guides are compiled, and run, as the doctests of the crate `quoll-guides`, in `guides/`:

```bash
cargo test -p quoll-guides
```
