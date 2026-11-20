# Errors

Every route returns a `rest::Result<T>`, whose error is `rest::Error`: one error for the server and the client. A
handler returns one, the server answers it as an RFC 9457 problem, and the client reads it back.

## In a handler

```rust
use http::StatusCode;
use quoll::rest::{self, Error, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        PUT "/notes/{id}" => rename(id: i64, title: String) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn rename(&self, id: i64, title: String) -> Result<String> {
        if title.is_empty() {
            return Err(Error::bad_request("a title is not empty"));
        }

        if id > 100 {
            return Err(Error::not_found("no such note"));
        }

        if id == 7 {
            return Err(Error::new(StatusCode::CONFLICT, "note 7 is being edited"));
        }

        let stored = store(id, &title).map_err(Error::internal)?;

        Ok(stored)
    }
}

fn store(id: i64, title: &str) -> std::io::Result<String> {
    Ok(format!("{id}: {title}"))
}
```

| Built with                   | The server answers                                   |
| ---------------------------- | ---------------------------------------------------- |
| `Error::bad_request(detail)` | 400, with the detail                                 |
| `Error::unauthorized(detail)`| 401, with the detail                                 |
| `Error::not_found(detail)`   | 404, with the detail                                 |
| `Error::new(status, detail)` | the status, with the detail                          |
| `Error::internal(cause)`     | 500, without the cause, which it logs                |

- `Error::new` takes a client or a server error, `4xx` or `5xx`. Any other status is answered 500, and logged.
- `Error::internal` takes any error. The server logs it through `tracing` and sends the status alone: the caller
  never sees the cause.
- An error that a client received from another API, carried up with `?`, is answered 502: the other API's status is
  not passed on as this one's.

## What the server answers

The body of an error is a problem, `application/problem+json`:

```json
{
  "type": "about:blank",
  "title": "Not Found",
  "status": 404,
  "detail": "no such note"
}
```

The server answers some errors itself, before or around the handler, each as a problem too:

- 400, an argument that does not read as its type: a path parameter, a query, a header, a body;
- 401, a route that takes a credential and gets none, or a wrong one, with a `WWW-Authenticate` challenge;
- 403, a caller that lacks a scope, or a request with a cookie scheme's cookie from another origin;
- 415, a body without the content type the route reads;
- 413, a body past `server::body_limit`, and 503, a handler past `server::timeout`.

The OpenAPI document lists, for each route, the errors quoll answers itself, and a `default` for any other.

## What a client sees

On the client, an answer with an error status is a `rest::Error` with that status. A call that got no answer to read
has none.

```rust
use http::StatusCode;
use quoll::rest::{self, Error, Result};
use quoll::{client, test};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, _id: i64) -> Result<String> {
        Err(Error::not_found("no such note"))
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let url = test::spawn(Api::router(ApiServices { notes: App })).await;
    let api = client::builder().build::<Api>(&url)?;

    let error = api.notes().get(9).await.unwrap_err();

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(error.problem().and_then(|problem| problem.detail()), Some("no such note"));
    assert_eq!(error.to_string(), "another api answered 404: no such note");
    Ok(())
}
```

- `status()` is the status of the answer, or `None` for a call that got no answer to read: a connection refused, a
  deadline passed, an error of a middleware, an answer that is not the route's result.
- `problem()` is the RFC 9457 problem, when the body is one.
- `body()` is the body as it came, its first 64 KiB.
- The error's sources show the URL of the call by its origin and its path alone: never its credentials, nor its
  query, where a query scheme sends its key.

A caller tells a status it expects from the others:

```rust,no_run
use http::StatusCode;
use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

#[tokio::main]
async fn main() -> rest::Result<()> {
    let api = Api::client("http://localhost:8080")?;

    match api.notes().get(999).await {
        Err(error) if error.status() == Some(StatusCode::NOT_FOUND) => println!("no note 999"),
        found => println!("{}", found?),
    }

    Ok(())
}
```

## Another API's errors

An API that quoll did not write may answer its errors in a shape of its own. `body_as::<T>()` reads the body as the
JSON of `T`, or gives `None`:

```rust,no_run
use quoll::rest;
use serde::{Deserialize, Serialize};

/// The body of Docker's errors, which are not RFC 9457 problems.
#[derive(Debug, Serialize, Deserialize)]
pub struct DockerError {
    pub message: String,
}

rest::api! {
    #[openapi(none)]
    pub api Docker = "/v1.47";

    pub group Containers {
        POST "/containers/{id}/start" => start(id: String);
    }
}

fn said(error: &rest::Error) -> String {
    match error.body_as::<DockerError>() {
        Some(DockerError { message }) => message,
        None => error.to_string(),
    }
}

#[tokio::main]
async fn main() -> rest::Result<()> {
    let docker = Docker::client("http://localhost:2375")?;

    if let Err(error) = docker.containers().start("web".into()).await {
        eprintln!("{}", said(&error));
    }

    Ok(())
}
```

It reads the extension members of a problem too.

## Errors in a stream

Once a stream's 200 is sent, an error ends it: an `Events` stream with a last `event: problem`, which the client reads
as that error, and JSON Lines or bytes by cutting the answer short. [Streams](contract.md#streams) says more.
