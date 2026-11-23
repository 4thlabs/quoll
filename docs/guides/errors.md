# Errors

Every route returns a `rest::Result<T>`, whose error is `rest::Error`: one error for the server and the client. A
handler returns one, the server answers it as an RFC 9457 problem, and the client reads it back. The client of
another API reads its errors in the shape the contract says, `#[error(E)]`.

## In a handler

```rust
use http::StatusCode;
use quoll::rest::{self, Error, Response, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        PUT "/notes/{id}" => rename(id: i64, title: String) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn rename(&self, id: i64, title: String) -> Result<Response<String>> {
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

        Ok(Response::new(stored))
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
use quoll::rest::{self, Error, Response, Result};
use quoll::{client, test};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, _id: i64) -> Result<Response<String>> {
        Err(Error::not_found("no such note"))
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let url = test::spawn(Api::router(ApiServices { notes: App })).await;
    let api = client::builder().build::<Api>(&url)?;

    let error = api.notes().get(9).await.unwrap_err();

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(error.detail().and_then(|problem| problem.detail()), Some("no such note"));
    assert_eq!(error.to_string(), "another api answered 404: no such note");
    Ok(())
}
```

- `status()` is the status of the answer, or `None` for a call that got no answer to read: a connection refused, a
  deadline passed, an error of a middleware, an answer that is not the route's result.
- `detail()` is the RFC 9457 problem, when the body is one, `application/problem+json`.
- `response()` is the answer with that detail, its status and its headers.
- `body()` is the body as it came, its first 64 KiB.
- The error's sources show the URL of the call by its origin and its path alone: never its credentials, nor its
  query, where a query scheme sends its key. A middleware of another crate that wraps reqwest's error in a type of its
  own, as reqwest-retry does, keeps the whole URL there: `client::retry` takes its place.

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

The headers of an error's answer, a `Retry-After` or a rate limit, come with the whole answer, which a call's
`.response()` gives: its body is then the error. [The whole answer](client.md#the-whole-answer) shows it.

## Another API's errors

An API that quoll did not write answers its errors in a shape of its own. `#[error(E)]` on its api says which: each
call's error is then a `rest::Error<E>`, whose `detail()` is the body of the answer read as the JSON of `E`. `E`
implements `Deserialize`, `Debug`, and `Display`, which gives the error's message.

```rust,no_run
use std::fmt;

use quoll::rest;
use serde::Deserialize;

/// The body of Docker's errors, which are not RFC 9457 problems.
#[derive(Debug, Deserialize)]
pub struct DockerError {
    pub message: String,
}

impl fmt::Display for DockerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

rest::api! {
    #[openapi(none)]
    #[error(DockerError)]
    pub api Docker = "/v1.47";

    pub group Containers {
        POST "/containers/{id}/start" => start(id: String);
    }
}

#[tokio::main]
async fn main() -> rest::Result<()> {
    let docker = Docker::client("http://localhost:2375")?;

    if let Err(error) = docker.containers().start("web".into()).await {
        // another api answered 404: No such container: web
        eprintln!("{error}");

        if let Some(DockerError { message }) = error.detail() {
            eprintln!("Docker says: {message}");
        }
    }

    Ok(())
}
```

- The calls return `rest::Result<T, E>`, an alias of `Result<T, rest::Error<E>>`. `rest::Error` is
  `rest::Error<rest::Problem>`, what a route without `#[error]` has.
- `detail()` is the `E`, `response()` the answer with it, its status and its headers, a rate limit or a
  `Retry-After`.
- A body that is not an `E`, a page of HTML from a proxy, or a 304, has no detail: the error keeps its status and its
  `body()`, and says `another api answered 502`.
- `?` carries a `rest::Error<E>` up into a `rest::Error`, a handler's or a `main`'s, with its status, its message and
  its body; a handler that returns it answers 502.
- `body_as::<T>()` reads the body as the JSON of another type: the extension members of a problem.

`#[error(E)]` goes on the api, a group or a route, the nearest to a route winning, as `#[auth]` does: a route that
answers another shape, an OAuth endpoint, says its own, and `#[error(rest::Problem)]` gives a group problems again. A
group of a `rest::group!` says its own, on the group or on each route: its trait is written before an api lists it, so
it cannot take the api's, and an api with an `#[error]` that lists a group with a route that has none does not
compile, unless the api's is `rest::Problem`.

```rust,no_run
use std::fmt;

use quoll::rest;
use serde::Deserialize;

/// What GitHub answers on an error.
#[derive(Debug, Deserialize)]
pub struct GitHubError {
    pub message: String,
}

impl fmt::Display for GitHubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

/// What GitHub's OAuth endpoint answers on an error.
#[derive(Debug, Deserialize)]
pub struct OAuthError {
    pub error: String,
    pub error_description: String,
}

impl fmt::Display for OAuthError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.error, self.error_description)
    }
}

rest::api! {
    #[openapi(none)]
    #[error(GitHubError)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}/topics" => topics(owner: String, repo: String) -> Vec<String>;

        #[error(OAuthError)]
        POST "/login/oauth/access_token" => token(code: String) -> String;
    }
}

#[tokio::main]
async fn main() -> rest::Result<()> {
    let github = GitHub::client("https://github.com")?;

    let topics: rest::Result<Vec<String>, GitHubError> =
        github.repos().topics("rust-lang".into(), "rust".into()).await;
    let token: rest::Result<String, OAuthError> = github.repos().token("code".into()).await;

    println!("{:?}", topics?);
    println!("{}", token?);
    Ok(())
}
```

A fake of such an api returns an `impl Call<T, E>`, and builds the error of an answer with
`rest::Error::answered(status, e)`: its status, its detail, a `response()` with no headers, and no body. An error of a
status alone, with no detail, is `rest::Error::not_found(..).into_shape()`, and so are the others.

```rust
use std::fmt;

use http::StatusCode;
use quoll::rest::{self, Call};
use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct DockerError {
    pub message: String,
}

impl fmt::Display for DockerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

rest::api! {
    #[openapi(none)]
    #[error(DockerError)]
    pub api Docker = "/v1.47";

    pub group Containers {
        POST "/containers/{id}/start" => start(id: String);
    }
}

struct NoContainers;

impl Containers for NoContainers {
    fn start(&self, id: String) -> impl Call<(), DockerError> {
        async move {
            let missing = DockerError {
                message: format!("No such container: {id}"),
            };

            Err(rest::Error::answered(StatusCode::NOT_FOUND, missing))
        }
    }
}

#[tokio::main]
async fn main() {
    let error = NoContainers.start("web".into()).await.unwrap_err();

    assert_eq!(error.to_string(), "another api answered 404: No such container: web");
}
```

Whatever `#[error]` says, quoll's own server answers problems, and the OpenAPI document describes them: the
attribute is for the contract of another API. The items of a stream keep `rest::Error`.

## Errors in a stream

Once a stream's 200 is sent, an error can no longer be its status: [Streams](contract.md#streams) says how each kind
of stream ends on an error, and what the client reads.
