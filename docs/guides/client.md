# The client

With the feature `client`, a contract gives a trait per group, which the client implements, and `ApiClient`, the
client of the api. `quoll::client::builder()` builds it with its middleware, on reqwest-middleware.

## A client

```rust,no_run
use std::time::Duration;

use quoll::client;
use quoll::rest::{self, Created};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
}

rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

#[tokio::main]
async fn main() -> rest::Result<()> {
    let api = client::builder()
        .with(client::user_agent("notes-cli/0.1"))
        .with(client::request_id())
        .with(client::retry(3))
        .with(client::timeout(Duration::from_secs(10)))
        .with(client::bearer("mF_9.B5f-4.1JqM"))
        .build::<Api>("http://localhost:8080")?;

    let created = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
        })
        .await?;
    let read = api.notes().get(created.id).await?;

    println!("{} {}", read.id, read.title);

    api.notes().remove(read.id).await?;
    Ok(())
}
```

## Calls

- `api.notes()` is the client of the group `Notes`, which implements the trait `Notes`: a method per route, its
  arguments in the contract's order.
- A route that returns `Created<T>` or `Accepted<T>` returns the `T` on the client.
- A query parameter that is `None` is left out, and a header that is `None` is not sent.
- Rust has no named arguments: a route with many optional keys takes a [query struct](contract.md#query-structs),
  which the caller fills with `..Default::default()`.
- Each call returns a `rest::Result`. [Errors](errors.md#what-a-client-sees) says what its error holds.

## Building a client

- `Api::client(url)` makes a client with no middleware.
- `client::builder()...build::<Api>(url)` makes one with middleware.
- `Api::client_with(http, url)` takes a `reqwest_middleware::ClientWithMiddleware` built already.

`url` is the base of the server, `http://localhost:8080`: the client writes each route's path after it, the api's
prefix included. Building fails on a URL that is not `http` or `https`, or that has a query.

`build_http()` gives the `ClientWithMiddleware` itself: one for the clients of several contracts, which then share
their connections.

```rust,no_run
use quoll::client;
use quoll::rest;

rest::api! {
    pub api Notes = "/api";

    pub group Search {
        GET "/search?q={q}" => search(q: String) -> Vec<String>;
    }
}

rest::api! {
    pub api Files = "/files";

    pub group Uploads {
        GET "/uploads" => list() -> Vec<String>;
    }
}

fn clients() -> rest::Result<(NotesClient, FilesClient)> {
    let http = client::builder().with(client::retry(3)).build_http()?;

    let notes = Notes::client_with(http.clone(), "https://notes.example.com")?;
    let files = Files::client_with(http, "https://files.example.com")?;

    Ok((notes, files))
}
```

## Middleware

`with` takes quoll's helpers, and any `reqwest_middleware::Middleware`:

| Middleware                  | What it does                                                                     |
| --------------------------- | -------------------------------------------------------------------------------- |
| `client::trace()`           | a tracing span per call, named by its operation                                  |
| `client::user_agent(name)`  | `User-Agent`, on a request that has none                                         |
| `client::request_id()`      | a new `X-Request-Id` per call, one for all its attempts                          |
| `client::retry(times)`      | tries a call again after a failure that may pass                                 |
| `client::timeout(duration)` | a deadline per attempt and per redirect                                          |
| `client::redirects(hops)`   | follows redirects one hop at a time, through the middleware after it             |
| `client::bearer(token)`     | `Authorization: Bearer`, on the routes that take `Bearer`                        |
| `client::bearer_from(src)`  | the same, with a token asked of a `TokenSource` before each call, and after a 401 |
| `client::basic(user, pwd)`  | `Authorization: Basic`, on the routes that take `Basic`                          |
| `client::key::<K>(value)`   | the credential of a scheme `K` the contract declares, where it travels           |
| `client::on_request(f)`     | a closure that changes each request before it leaves                             |

The builder puts quoll's helpers in the order they work in, whatever the order they are given in, from the first,
which sees the request first: `trace`, `user_agent`, `request_id`, `retry`, `timeout`, `redirects`, then the
credentials and `on_request` in the order given. Any other middleware keeps the place it is given at. A middleware of
the application's that adds a credential is given after `redirects`.

Each helper is a `reqwest_middleware::Middleware` too, for an application that builds its `ClientWithMiddleware`
itself. There, they run in the order they are added: add them in the order above.

Each request of a contract carries its `rest::Route` in its extensions, which a middleware of the application's
reads: the operation id, the group, the method, the path as written, and the schemes it takes.

## Credentials

The helpers of authentication send a credential on the routes that take its scheme, and on no other request: not on a
route with `#[auth(none)]`, not on a request to another origin than the api's, nor on a redirect there.

```rust,no_run
use quoll::client;
use quoll::rest;

rest::api! {
    pub api Api = "/api";

    /// The key of an operator's scripts.
    pub scheme Key = header "X-Api-Key";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes" => list() -> Vec<String>;
    }

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> u64;
    }
}

fn client(token: &str, key: &str) -> rest::Result<ApiClient> {
    client::builder()
        .with(client::bearer(token))
        .with(client::key::<Key>(key))
        .build::<Api>("http://localhost:8080")
}
```

- `bearer` and `basic` send `Authorization`, marked sensitive so that a log does not show it.
- `key::<K>` sends the credential of a declared scheme where it travels: its header, its cookie, its query key, or
  `Authorization: Bearer` for an `openid` scheme. A scheme is told by its type: two contracts that each declare a
  `Key` each get their own.
- A credential the server would not read, a token with a space or a key with a line break, fails each call that would
  send it, with an error that does not show it.

[Authentication](authentication.md) covers the schemes, and what the server does with each.

### Tokens that expire

`bearer_from` asks a `client::TokenSource` for a token before each call. A call answered 401 is sent once more with
the token `refresh` gives, and that second answer is the call's.

```rust,no_run
use quoll::client::{self, TokenSource};
use quoll::rest;
use tokio::sync::Mutex;

/// The token of a session, which a call to the authentication server renews.
struct Session {
    token: Mutex<String>,
}

impl TokenSource for Session {
    async fn token(&self) -> rest::Result<String> {
        Ok(self.token.lock().await.clone())
    }

    async fn refresh(&self, refused: &str) -> rest::Result<String> {
        let mut token = self.token.lock().await;

        // Another call has renewed it since this one sent `refused`.
        if *token != refused {
            return Ok(token.clone());
        }

        *token = renew().await?;
        Ok(token.clone())
    }
}

async fn renew() -> rest::Result<String> {
    Ok("a new token".into())
}

fn http() -> rest::Result<reqwest_middleware::ClientWithMiddleware> {
    let session = Session {
        token: Mutex::new("t-1".into()),
    };

    client::builder().with(client::bearer_from(session)).build_http()
}
```

The call is not sent again when `refresh` gives back the token refused. An error of the source fails the call.

### A header computed from the request

`on_request` runs a closure on each request of a contract to its api's own origin, given the request and its
`rest::Route`: a signature, a header of the application's.

```rust,no_run
use quoll::{client, rest};
use reqwest::Request;

fn signature(method: &http::Method, path: &str, body: &[u8]) -> http::HeaderValue {
    let text = format!("{method} {path} {}", body.len());

    http::HeaderValue::from_str(&text).expect("a method, a path and a number make a header value")
}

fn http() -> rest::Result<reqwest_middleware::ClientWithMiddleware> {
    let sign = client::on_request(|request: &mut Request, _route: &rest::Route| {
        let body = request.body().and_then(|body| body.as_bytes()).unwrap_or_default();
        let value = signature(request.method(), request.url().path(), body);

        request.headers_mut().insert("X-Signature", value);
    });

    client::builder().with(sign).build_http()
}
```

## Retries, deadlines and redirects

- `retry(times)` tries a call again after a failure that may pass: no answer, an answer that did not begin before the
  deadline of `timeout`, or a 502, 503 or 504. It waits a random backoff between attempts. It tries again `GET`,
  `HEAD`, `PUT` and `DELETE` only: a `POST` may have been done before its connection was cut.
- `timeout(duration)` is a deadline per attempt and per hop, from the request to the end of the answer's body. On a
  route whose result is a stream, it bounds the head of the answer alone.
- `redirects(hops)` follows up to `hops` redirects, each hop through the middleware after it. A hop to another origin
  carries no credential.

Given a credential, `bearer`, `bearer_from`, `basic`, `key` or `on_request`, and no `redirects`, the builder adds
`redirects(10)` itself, so that redirects are still followed, and no hop to another origin carries a credential.
Without either, reqwest follows redirects as it does by default.

The docs of `quoll::client::retry`, `timeout` and `redirects` say how each counts and what each follows.

## reqwest's client

`.http(|http| ...)` configures reqwest's own `ClientBuilder`, under the middleware: a proxy, TLS, a pool.

```rust,no_run
use std::time::Duration;

use quoll::client;

fn http() -> Result<reqwest_middleware::ClientWithMiddleware, Box<dyn std::error::Error>> {
    let proxy = reqwest::Proxy::all("http://proxy.example.com:3128")?;

    let http = client::builder()
        .http(move |http| http.proxy(proxy))
        .http(|http| http.pool_idle_timeout(Duration::from_secs(30)))
        .build_http()?;

    Ok(http)
}
```

With `redirects`, given or added, reqwest's client follows no redirect itself, whatever `http(..)` sets.

### A Unix socket

A server that listens on a Unix socket, as the Docker daemon does, is reached through reqwest's `unix_socket`. The
URL's host is then only the request's `Host`:

```rust,no_run
use quoll::{client, rest};
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct Version {
    #[serde(rename = "Version")]
    pub version: String,
}

rest::api! {
    #[openapi(none)]
    pub api Docker = "/v1.47";

    pub group System {
        GET "/version" => version() -> Version;
    }
}

#[tokio::main]
async fn main() -> rest::Result<()> {
    let docker = client::builder()
        .http(|http| http.unix_socket("/var/run/docker.sock"))
        .build::<Docker>("http://localhost")?;

    let version = docker.system().version().await?;

    println!("Docker {}", version.version);
    Ok(())
}
```

## Fakes

The trait of a group, `Notes`, is implemented by the client and by a fake. Code written for `&impl Notes` takes
either: [Testing](testing.md#fakes) shows one.
