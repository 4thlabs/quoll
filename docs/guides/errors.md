# Errors

Every route returns a `quoll::Result<T>`, whose error is `quoll::Error`: one error for the server and the client. A
handler returns one, the server answers it as an RFC 9457 problem, and the client reads it back. The client of
another API reads its errors in the shape that the contract says, with `#[error(E)]`.

## In a handler

```rust
use http::StatusCode;
use quoll::{Error, Response, Result};

quoll::api! {
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
- An error that a client received from another API, carried up with `?`, is answered 502. The other API's status is
  not passed on as the status of this API.

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

For each route, the OpenAPI document lists the errors that quoll answers itself, and a `default` for any other.

## What a client sees

On the client, an answer with an error status is a `quoll::Error` with that status. A call that got no answer to read
has none: its kind says what went wrong.

```rust
use http::StatusCode;
use quoll::{Error, Response, Result, client, test};

quoll::api! {
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

- `kind()` is what the error is, a `quoll::ErrorKind`: `ErrorKind::Status` of its status, or what went wrong in a
  call that got no answer to read. [The kind of an error](#the-kind-of-an-error) lists them.
- `status()` is the status of the answer, or `None` for a call that got no answer to read: a connection refused, a
  deadline passed, an error of a middleware, or an answer that is not the route's result. A middleware that turns an
  answer into an error, with `error_for_status`, fails the call with no status. `status()` is `Some(status)` exactly
  when `kind()` is `ErrorKind::Status(status)`.
- `detail()` is the RFC 9457 problem, when the body is one (`application/problem+json`).
- `response()` is the answer, with that detail, its status and its headers.
- `body()` is the body as it came: its first 64 KiB.
- The error's sources show the URL of the call by its origin and its path alone. They never show its credentials, or
  its query, where a query scheme sends its key. A middleware of another crate may wrap reqwest's error in a type of
  its own, like reqwest-retry does. That type keeps the whole URL: `client::retry` takes the place of reqwest-retry.

A caller tells a status it expects apart from the others:

```rust,no_run
use http::StatusCode;

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let api = Api::client("http://localhost:8080")?;

    match api.notes().get(999).await {
        Err(error) if error.status() == Some(StatusCode::NOT_FOUND) => println!("no note 999"),
        found => println!("{}", found?),
    }

    Ok(())
}
```

The headers of an error's answer, such as a `Retry-After` or a rate limit, come with the whole answer, which a call's
`.response()` gives. Its body is then the error. [The whole answer](client.md#the-whole-answer) shows it.

### The kind of an error

Every error has a kind, which `error.kind()` gives. A handler's errors have one too: `Error::not_found(..)` is
`ErrorKind::Status(StatusCode::NOT_FOUND)`, and `Error::internal(..)` is a 500.

| Kind             | What went wrong                                                                          |
|------------------|------------------------------------------------------------------------------------------|
| `Status(status)` | the answer has an error status; its `detail()` is the body, when it has the api's shape  |
| `Connect`        | no connection: refused, a name that does not resolve, TLS. Nothing was sent.             |
| `Timeout`        | a deadline passed. The other API may have served the request, or begun to answer it.     |
| `Closed`         | the request was sent, and no usable answer came back: closed, reset, or not HTTP         |
| `Body`           | the answer began, and its body could not be read to its end                              |
| `Decode`         | the answer has a success status, and is not the route's result                           |
| `Middleware`     | a middleware failed the call; a reqwest error it passes on follows the rule below        |
| `Invalid`        | the call could not be sent as it is written, such as a URL that is not one               |

A middleware's error says why among its sources. A reqwest error, or an error of reqwest-middleware's, that a
middleware passes on as it is keeps its kind, whichever client it came from: the rest of the call's chain, or a client
of the middleware's own, which the kind cannot tell apart. A middleware whose own failure must read `Middleware` wraps
it in an error type of its own. A reqwest error passed on is `Middleware` when it is a body, decode, redirect,
upgrade or status error, and keeps `Connect`, `Timeout`, `Invalid` or `Closed` otherwise. A body that quoll reads
itself and that fails is `Body`. quoll's deadline on the head of a stream is `Timeout`.

`ErrorKind` is `#[non_exhaustive]`: a `match` on it ends with `_`, so that a later version may add a kind. A status is
matched by its constant, or by a guard:

```rust,no_run
use http::StatusCode;
use quoll::ErrorKind;

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let api = Api::client("http://localhost:8080")?;

    let Err(error) = api.notes().get(999).await else {
        return Ok(());
    };

    match error.kind() {
        ErrorKind::Status(StatusCode::NOT_FOUND) => println!("no note 999"),
        ErrorKind::Status(status) if status.is_server_error() => println!("the server is in trouble"),
        ErrorKind::Connect => println!("nothing listens there"),
        ErrorKind::Timeout => println!("the server is slow: try again later"),
        _ => return Err(error),
    }

    Ok(())
}
```

`kind.as_str()` gives the code of a status, `404`, or the kind's name in lowercase, `timeout`. `client::trace` records
it as `error.type` on the span of a call that failed. `kind.answered()` says whether the other API answered: it is
true for `Status`, `Body` and `Decode`.

## Another API's errors

An API that quoll did not write answers its errors in a shape of its own. `#[error(E)]` on its api says which shape.
Each call's error is then a `quoll::Error<E>`, whose `detail()` is the body of the answer, read as the JSON of `E`. `E`
implements `Deserialize`, `Debug`, and `Display`, which gives the error's message.

```rust,no_run
use std::fmt;

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

quoll::api! {
    #[openapi(none)]
    #[error(DockerError)]
    pub api Docker = "/v1.47";

    pub group Containers {
        POST "/containers/{id}/start" => start(id: &str);
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let docker = Docker::client("http://localhost:2375")?;

    if let Err(error) = docker.containers().start("web").await {
        // another api answered 404: No such container: web
        eprintln!("{error}");

        if let Some(DockerError { message }) = error.detail() {
            eprintln!("Docker says: {message}");
        }
    }

    Ok(())
}
```

- The calls return `quoll::Result<T, E>`, an alias of `Result<T, quoll::Error<E>>`. `quoll::Error` is
  `quoll::Error<quoll::Problem>`, the error of a route without `#[error]`.
- `detail()` is the `E`. `response()` is the answer with it, its status and its headers, such as a rate limit or a
  `Retry-After`.
- A body that is not an `E`, such as a page of HTML from a proxy, has no detail, and neither has a 304. The error keeps
  its status and its `body()`, and says `another api answered 502`.
- `?` carries a `quoll::Error<E>` up into a `quoll::Error`, such as the error of a handler or of `main`, with its
  status, its message and its body. A handler that returns it answers 502.
- `body_as::<T>()` reads the body as the JSON of another type, such as the extension members of a problem.

`#[error(E)]` goes on the api, a group or a route, and the one nearest to a route wins, like `#[auth]`. A route that
answers another shape, such as an OAuth endpoint, says its own. `#[error(quoll::Problem)]` gives a group problems
again. A group of a `quoll::group!` says its own `#[error]`, on the group or on each route. Its trait is written before
an api lists it, so it cannot take the `#[error]` of the api. So an api with an `#[error]` does not compile when it
lists a group with a route that has none, unless the api says `#[error(quoll::Problem)]`.

```rust,no_run
use std::fmt;

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

quoll::api! {
    #[openapi(none)]
    #[error(GitHubError)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}/topics" => topics(owner: &str, repo: &str) -> Vec<String>;

        #[error(OAuthError)]
        POST "/login/oauth/access_token" => token(code: String) -> String;
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let github = GitHub::client("https://github.com")?;

    let topics: quoll::Result<Vec<String>, GitHubError> =
        github.repos().topics("rust-lang", "rust").await;
    let token: quoll::Result<String, OAuthError> = github.repos().token("code".into()).await;

    println!("{:?}", topics?);
    println!("{}", token?);
    Ok(())
}
```

A fake of such an api returns an `impl Call<T, E>`. It builds the error of an answer with
`quoll::Error::answered(status, e)`. That error has its status, its detail, a `response()` with no headers, and no
body. An error of a status alone, with no detail, is `quoll::Error::not_found(..).into_shape()`, and the same goes for
the other constructors.

```rust
use std::fmt;

use http::StatusCode;
use quoll::Call;
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

quoll::api! {
    #[openapi(none)]
    #[error(DockerError)]
    pub api Docker = "/v1.47";

    pub group Containers {
        POST "/containers/{id}/start" => start(id: &str);
    }
}

struct NoContainers;

impl Containers for NoContainers {
    fn start(&self, id: &str) -> impl Call<(), DockerError> {
        async move {
            let missing = DockerError {
                message: format!("No such container: {id}"),
            };

            Err(quoll::Error::answered(StatusCode::NOT_FOUND, missing))
        }
    }
}

#[tokio::main]
async fn main() {
    let error = NoContainers.start("web").await.unwrap_err();

    assert_eq!(error.to_string(), "another api answered 404: No such container: web");
}
```

Whatever `#[error]` says, quoll's own server answers problems, and the OpenAPI document describes them: the
attribute is for the contract of another API. The items of a stream keep `quoll::Error`.

## Errors in a stream

Once a stream's 200 is sent, an error can no longer be its status. [Streams](contract.md#streams) says how each kind
of stream ends on an error, and what the client reads.

On the client, the kind of an item's error says whether the stream goes on:

- after `ErrorKind::Decode`, an item that does not read as the route's item, the next item follows;
- after any other kind, the stream has ended, and the next item is `None`. An `event: problem` is the error of an
  answer, `ErrorKind::Status` of the problem's status, with the problem as its `detail()`. A body cut short is `Body`.

```rust,no_run
use futures::StreamExt;
use quoll::{ErrorKind, JsonLines};
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct Change {
    pub id: i64,
}

quoll::api! {
    #[openapi(none)]
    pub api Api = "/api";

    pub group Changes {
        GET "/changes" => follow() -> JsonLines<Change>;
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let api = Api::client("http://localhost:8080")?;
    let mut changes = api.changes().follow().await?;

    while let Some(change) = changes.next().await {
        match change {
            Ok(change) => println!("{change:?}"),
            // A change of a kind newer than this code: skip it, the stream goes on.
            Err(error) if error.kind() == ErrorKind::Decode => eprintln!("skipped: {error}"),
            Err(error) => return Err(error),
        }
    }

    Ok(())
}
```
