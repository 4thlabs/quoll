# The client

With the feature `client`, a contract gives one trait per group, which the client implements. It also gives
`ApiClient`, the client of the api. `quoll::client::builder()` builds it with its middleware, on reqwest-middleware.

## A client

```rust,no_run
use std::time::Duration;

use quoll::{Created, client};
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

quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
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

- `api.notes()` is the client of the group `Notes`, which implements the trait `Notes`. It has one method per route,
  with its arguments in the contract's order.
- An argument the contract writes with a `&`, as `owner: &str`, is borrowed: `get("rust-lang", &repo)`. The call may
  borrow it until it is awaited. [Borrowed arguments](contract.md#borrowed-arguments) lists the forms.
- A route that returns `Created<T>` or `Accepted<T>` returns the `T` on the client.
- A query parameter that is `None` is left out, and a header that is `None` is not sent.
- Rust has no named arguments: a route with many optional keys takes a [query struct](contract.md#query-structs),
  which the caller fills with `..Default::default()`.
- Each call returns a `quoll::Call`. `.await` sends it and gives a `quoll::Result`.
  [Errors](errors.md#what-a-client-sees) says what its error holds.

## The whole answer

Each method returns a `quoll::Call`: the call described, and not sent yet. `.await` sends it and gives the route's
value, or an error that goes up. Before that, its methods change it:

- `.response()` gives the whole answer, whatever its status. That is its `status()`, its `headers()` (for example a
  rate limit or a `Retry-After`), and its body. The body is the route's value on a success, and the error of the
  answer otherwise. `.into_value()` gives the body, to go up with `?`.
- `.timeout(duration)` is a deadline for this call alone, in place of the client's `client::timeout`: see
  [Retries, deadlines and redirects](#retries-deadlines-and-redirects).

```rust,no_run
use std::time::Duration;

use http::StatusCode;
use quoll::Call;

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

async fn find(api: &ApiClient, id: i64) -> quoll::Result<Option<String>> {
    let response = api
        .notes()
        .get(id)
        .timeout(Duration::from_secs(5))
        .response()
        .await?;

    if let Some(remaining) = response.headers().get("x-ratelimit-remaining") {
        println!("{remaining:?} calls left");
    }

    if response.status() == StatusCode::NOT_FOUND {
        return Ok(None);
    }

    Ok(Some(response.into_value()?))
}
# let _ = find;
```

The `Err` of `.response()` is a call that got no answer it can read: a connection refused, a deadline passed, or an
answer with a success status that is not the route's result.

- A 204 or a 205 is a success on a route whose value may be empty: `()`, `None`, an empty text or bytes, or a stream
  that ends at once. Another API may answer one on a route whose value must be there. Then the `.response()` keeps
  its status and its headers, and its value is the error of a body that is not the result.
- A 304 Not Modified, which answers a call that sent `If-None-Match`, is an error of its own. It holds the status and
  the headers of the `.response()`, with no [detail](errors.md#another-apis-errors), and `.into_value()` gives it.
  `.await` gives that error too.

## Building a client

- `Api::client(url)` makes a client with no middleware.
- `client::builder()...build::<Api>(url)` makes one with middleware.
- `Api::client_with(http, url)` takes a `reqwest_middleware::ClientWithMiddleware` that is already built.

`url` is the base of the server, such as `http://localhost:8080`. The client writes each route's path after it,
including the api's prefix. Building fails on a URL that is not `http` or `https`, or that has a query.

`build_http()` gives the `ClientWithMiddleware` itself. One of them can serve the clients of several contracts, which
then share their connections.

```rust,no_run
use quoll::client;

quoll::api! {
    pub api Notes = "/api";

    pub group Search {
        GET "/search?q={q}" => search(q: &str) -> Vec<String>;
    }
}

quoll::api! {
    pub api Files = "/files";

    pub group Uploads {
        GET "/uploads" => list() -> Vec<String>;
    }
}

fn clients() -> quoll::Result<(NotesClient, FilesClient)> {
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

The builder puts quoll's helpers in the order they work in, whatever order they are given in. From the first, which
sees the request first:

1. `trace`;
2. `user_agent`;
3. `request_id`;
4. `retry`;
5. `timeout`;
6. `redirects`;
7. the credentials and `on_request`, in the order given.

Any other middleware keeps the place it is given. An application's own middleware that adds a credential is given
after `redirects`.

Each helper is also a `reqwest_middleware::Middleware`, for an application that builds its `ClientWithMiddleware`
itself. There, the helpers run in the order they are added: add them in the order above.

Each request of a contract carries its `quoll::Route` in its extensions, which an application's own middleware reads.
The route holds:

- the operation id;
- the group;
- the method;
- the path as written;
- the schemes it takes.

## Credentials

The helpers of authentication send a credential on the routes that take its scheme, and on no other request. So they
do not send it on a route with `#[auth(none)]`, on a request to an origin other than the api's origin, or on a
redirect there.

```rust,no_run
use quoll::client;

quoll::api! {
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

fn client(token: &str, key: &str) -> quoll::Result<ApiClient> {
    client::builder()
        .with(client::bearer(token))
        .with(client::key::<Key>(key))
        .build::<Api>("http://localhost:8080")
}
```

- `bearer` and `basic` send `Authorization`, marked sensitive so that a log does not show it.
- `key::<K>` sends the credential of a declared scheme where it travels: its header, its cookie, its query key, or
  `Authorization: Bearer` for an `openid` scheme. A scheme is told apart by its type: two contracts that each declare
  a `Key` each get their own.
- A credential that the server would not read, such as a token with a space or a key with a line break, fails each
  call that would send it. The error does not show the credential.

[Authentication](authentication.md) covers the schemes, and what the server does with each.

### Tokens that expire

`bearer_from` asks a `client::TokenSource` for a token before each call. A call answered 401 is sent once more with
the token that `refresh` gives, and the call returns that second answer.

```rust,no_run
use quoll::client::{self, TokenSource};
use tokio::sync::Mutex;

/// The token of a session, which a call to the authentication server renews.
struct Session {
    token: Mutex<String>,
}

impl TokenSource for Session {
    async fn token(&self) -> quoll::Result<String> {
        Ok(self.token.lock().await.clone())
    }

    async fn refresh(&self, refused: &str) -> quoll::Result<String> {
        let mut token = self.token.lock().await;

        // Another call has renewed it since this one sent `refused`.
        if *token != refused {
            return Ok(token.clone());
        }

        *token = renew().await?;
        Ok(token.clone())
    }
}

async fn renew() -> quoll::Result<String> {
    Ok("a new token".into())
}

fn http() -> quoll::Result<reqwest_middleware::ClientWithMiddleware> {
    let session = Session {
        token: Mutex::new("t-1".into()),
    };

    client::builder().with(client::bearer_from(session)).build_http()
}
```

The call is not sent again when `refresh` gives back the refused token. An error from the source fails the call.

### A header computed from the request

`on_request` runs a closure on each request of a contract to its api's own origin. The closure is given the request
and its `quoll::Route`, and adds, for example, a signature or an application's own header. A header with the same
value on every request belongs to the api, and goes in its contract: `#[header("name" = value)]` (see
[Fixed headers](contract.md#fixed-headers)).

```rust,no_run
use quoll::client;
use reqwest::Request;

fn signature(method: &http::Method, path: &str, body: &[u8]) -> http::HeaderValue {
    let text = format!("{method} {path} {}", body.len());

    http::HeaderValue::from_str(&text).expect("a method, a path and a number make a header value")
}

fn http() -> quoll::Result<reqwest_middleware::ClientWithMiddleware> {
    let sign = client::on_request(|request: &mut Request, _route: &quoll::Route| {
        let body = request.body().and_then(|body| body.as_bytes()).unwrap_or_default();
        let value = signature(request.method(), request.url().path(), body);

        request.headers_mut().insert("X-Signature", value);
    });

    client::builder().with(sign).build_http()
}
```

## Retries, deadlines and redirects

- `retry(times)` tries a call again after a failure that may pass: no answer, an answer that did not begin before the
  deadline of `timeout`, or a 502, 503 or 504. It waits a random backoff between attempts. It tries again only `GET`,
  `HEAD`, `PUT` and `DELETE`: a `POST` may have been done before its connection was cut. `.when(..)` decides
  otherwise: see [Deciding a retry](#deciding-a-retry).
- `timeout(duration)` is a deadline per attempt and per hop, from the request to the end of the answer's body. On a
  route whose result is a stream, it bounds only the head of the answer.
- A call's `.timeout(duration)` is a deadline for that call alone, in place of the deadline of `timeout`, with the
  same meaning. A long call, such as a slow export, waits longer than the others. A client with no `timeout` takes
  it too.

```rust,no_run
use std::time::Duration;

use quoll::Call;

quoll::api! {
    pub api Api = "/api";

    pub group Exports {
        POST "/exports" => create(name: String) -> u64;
    }
}

async fn export(api: &ApiClient) -> quoll::Result<u64> {
    api.exports()
        .create("notes".into())
        .timeout(Duration::from_secs(600))
        .await
}
```

`redirects(hops)` follows up to `hops` redirects, each hop through the middleware after it. A hop to another origin
carries no credential.

Given a credential (`bearer`, `bearer_from`, `basic`, `key` or `on_request`) and no `redirects`, the builder adds
`redirects(10)` itself. So redirects are still followed, and no hop to another origin carries a credential. With
neither a credential nor `redirects`, reqwest follows redirects in its default way.

The docs of `quoll::client::retry`, `timeout` and `redirects` say how each counts and what each follows.

### Deciding a retry

`retry(times).when(..)` takes a closure, which is given each `client::Attempt`. An attempt holds the call's
`quoll::Route`, its request, then the answer's status and headers or the error, and the attempt's number. The closure
returns one of these:

- `Retry::No`, to keep the answer;
- `Retry::Backoff`, to try again after the backoff of `retry`;
- `Retry::After(duration)`, to try again after `duration`.

`attempt.kind()` is the attempt's kind, as the call's error would say it: `None` for an answer of a success status,
`ErrorKind::Status(status)` for any other answer, and `ErrorKind::Connect`, `Timeout`... for an attempt that got no
answer. [The kind of an error](errors.md#the-kind-of-an-error) lists them.

`attempt.transient()` is the rule of `retry(times)` alone, for the cases that the closure leaves.

`retry` reads the status of the answers that reach it. A middleware that turns answers into errors, with
`error_for_status`, goes before `retry` in the builder. Added after `retry`, the middleware hides the status from it:
`retry` sees the middleware's error, of the kind `Middleware`, which it does not try again.

`attempt.retry_after()` is the wait that the answer's `Retry-After` asks, in seconds or as an HTTP date. It is a minute
at most, unless `.max_wait(..)` sets another.

```rust,no_run
use std::time::Duration;

use http::{Method, StatusCode};
use quoll::client::{self, Attempt, Retry};

fn http() -> quoll::Result<reqwest_middleware::ClientWithMiddleware> {
    let retry = client::retry(3)
        .max_wait(Duration::from_secs(30))
        .when(|attempt: &Attempt| {
            let request = attempt.request();
            let keyed = request.method() == Method::POST
                && request.headers().contains_key("Idempotency-Key");

            match attempt.status() {
                // The server did not act on it: any method may be sent again.
                Some(StatusCode::TOO_MANY_REQUESTS) => attempt.retry_after(),
                // A POST the server does once per key.
                Some(status) if keyed && status.is_server_error() => Retry::Backoff,
                _ => attempt.transient(),
            }
        });

    client::builder().with(retry).build_http()
}
```

The request is the one that `retry` is given. The headers of its route are there, such as an `Idempotency-Key` that
the route takes as an argument. The headers added after `retry` are not: the credentials, and what `on_request` puts
in. Whatever the closure decides, a call is tried `times` more at most. A request whose body is a stream that the
application gives is sent once, without calling the closure.

## Following a stream

A stream route ends when its connection does: the server restarts, a proxy closes an idle connection, the network
goes down. `client::follow` calls the route again each time its stream ends or fails, after a wait. It takes an async
closure that makes the call and awaits it:

```rust,no_run
use futures::StreamExt;
use quoll::JsonLines;
use quoll::client::{self, Followed};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Event {
    #[serde(rename = "Type")]
    pub kind: String,
}

quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group System {
        GET "/events?filters={filters}" => events(filters: &str) -> JsonLines<Event>;
    }
}

async fn watch(docker: &DockerClient) -> quoll::Result<()> {
    let system = docker.system();
    let filters = r#"{"type":["container"]}"#.to_owned();

    let mut events = client::follow(async move || system.events(&filters).await);

    while let Some(followed) = events.next().await {
        match followed {
            Followed::Connected => println!("following: anything may have changed"),
            Followed::Item(event) => println!("a {} changed", event.kind),
            Followed::Skipped(error) => eprintln!("an event that does not read: {error}"),
            Followed::Reconnecting { error, wait } => {
                eprintln!("following again in {wait:?}: {error:?}");
            }
            Followed::Stopped(error) => return Err(error),
        }
    }

    Ok(())
}
```

The closure is `async move ||`. It owns the group's client and the arguments, and lends them to each call. The stream
that `follow` gives is then `'static`, and `Send`, so that a task can take it with `tokio::spawn`.

The stream gives a `Followed` for each thing that happens:

- `Connected` each time a call is answered with its stream. A first call that fails gives `Reconnecting`, or
  `Stopped`, before any `Connected`. Items may have been missed before each one, so a consumer that keeps a state
  reloads it there.
- `Item(T)` for each item.
- `Skipped(error)` for an item that does not read. The connection goes on.
- `Reconnecting { error, wait }` when the stream ended, `error` being `None`, or failed. The route is called again
  `wait` after this state is given, even if the consumer takes a while to read the next one.
- `Stopped(error)` when calling again would fail the same way. It is the last one, and the stream ends after it.

There is no `Result` around them, so a `?` on each item does not stop the consumer on an item that does not read.

### What reconnects and what stops

`follow` sorts each error by its [kind](errors.md#the-kind-of-an-error):

- the call answered with a 4xx other than 408 and 429, or with a 3xx, stops: a 404 or a 401 would come back again;
- the call that the client cannot send, of the kind `Invalid`, stops;
- any other failure of the call reconnects: no connection, a deadline, a connection closed, a 5xx, a 408, a 429, an
  answer that is not the route's, such as a proxy's page of HTML;
- in the stream, an item that does not read, of the kind `Decode`, is skipped;
- any other error of the stream drops the connection and reconnects, an `event: problem` too, whatever its status.

A 401 that reaches `follow` stops it, as `bearer_from` has already sent the call once more with a new token. A
`retry` of the client still runs inside each call.

### The waits

The first call is made at once. After it, each wait is drawn at random between half a bound and the bound. The bound
starts at 1 s and doubles after each wait, up to 30 s. A connection that stays open 10 s has held: the bound after it
starts again from 1 s. Items do not count, only the time: a server that sends an item and then closes each connection
is called less and less often. An item read after the 10 s holds the connection, even when the consumer is behind.

A zero `first` calls again at once the first time, then the bound doubles from 1 ms. A `longest` below 1 ms is raised
to 1 ms, and a `first` above `longest` is cut to it. A `steady` of `Duration::MAX` never counts a connection as held.

`client::Reconnect` sets them:

```rust,no_run
use std::time::Duration;

use quoll::ByteStream;
use quoll::client::{Followed, Reconnect};

quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group Containers {
        GET "/containers/{id}/logs?follow={follow}" => logs(id: &str, follow: bool) -> ByteStream;
    }
}

fn logs(docker: &DockerClient, id: String) -> impl futures::Stream<Item = Followed<Vec<u8>>> {
    let containers = docker.containers();

    Reconnect::new()
        .first(Duration::from_millis(500))
        .longest(Duration::from_secs(10))
        .steady(Duration::from_secs(30))
        .follow(async move || containers.logs(&id, true).await)
}
```

`without_jitter()` makes each wait its bound, for a test that checks the times.

### Per call, and in tests

What is set per call goes in the closure: `.timeout(..)` for the head of each call, or `.response()` to read the
headers of each answer before its stream. The closure may also compute an argument at each call, such as where to
start from.

`follow` calls the method of the group's trait, so it follows a fake as it follows the client. A fake that answers
each call in its own way scripts the connections of a test. Under `#[tokio::test(start_paused = true)]`, the waits
take no time.

## reqwest's client

`.http(|http| ...)` configures reqwest's own `ClientBuilder`, under the middleware: for example a proxy, TLS or a
pool.

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

A server that listens on a Unix socket, such as the Docker daemon, is reached through reqwest's `unix_socket`. The
URL's host is then only the request's `Host`:

```rust,no_run
use quoll::client;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct Version {
    #[serde(rename = "Version")]
    pub version: String,
}

quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group System {
        GET "/version" => version() -> Version;
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
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
