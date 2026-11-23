# Authentication

The contract says which credential a group or a route takes, with `#[auth(...)]`. The server says how a credential
becomes a caller, and which type that caller is. The client sends the credential through its middleware.

## A route that takes a credential

```rust
use quoll::{Authenticate, Bearer, Error, Response, Result, client, test};

quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

/// Who a request comes from, once its token is checked.
struct User {
    name: String,
}

struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        match token.as_str() {
            "ana-token" => Ok(User { name: "ana".into() }),
            _ => Err(Error::unauthorized("unknown or expired token")),
        }
    }
}

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64) -> Result<Response<String>> {
        Ok(Response::new(format!("note {id} of {}", user.name)))
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let url = test::spawn(Api::router(ApiServices { notes: App })).await;

    let api = client::builder()
        .with(client::bearer("ana-token"))
        .build::<Api>(&url)?;

    assert_eq!(api.notes().get(1).await?, "note 1 of ana");

    let anonymous = Api::client(&url)?;
    let error = anonymous.notes().get(1).await.unwrap_err();

    assert_eq!(error.status(), Some(http::StatusCode::UNAUTHORIZED));
    Ok(())
}
```

- The handler receives the caller after `&self`, before the route's own arguments. The client's trait has no caller.
- A service whose group takes `Bearer` implements `Authenticate<Bearer>`: one that does not, does not compile.
- `authenticate` runs before the handler, and before the request's body is read.
- The contract names no type of the caller: it lives on the server.

## What a route takes

| On a group or a route              | The handler receives            | Without a valid credential                       |
| ---------------------------------- | ------------------------------- | ------------------------------------------------ |
| `#[auth(Bearer)]`                  | `&Caller`                       | 401                                              |
| `#[auth(Bearer \| Key)]`           | `&Caller`, from either scheme   | 401                                              |
| `#[auth(Bearer, optional)]`        | `Option<&Caller>`               | `None` without a credential, 401 with a wrong one |
| `#[auth(Bearer, scopes("repo"))]`  | `&Caller`                       | 401, or 403 when the caller lacks a scope        |
| `#[auth(none)]`, on a route        | nothing                         | the route leaves its group's authentication     |

A route's attribute replaces its group's.

```rust
quoll::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;

        #[auth(none)]
        GET "/public" => public() -> Vec<String>;

        #[auth(Bearer, optional)]
        GET "/notes/{id}/likes" => likes(id: i64) -> u32;

        #[auth(Bearer | Key)]
        GET "/count" => count() -> u64;

        #[auth(Bearer, scopes("admin"))]
        DELETE "/notes/{id}" => remove(id: i64);
    }
}
```

### Errors of `authenticate`

The error of `authenticate` answers its own status, and the handler is not called:

- `Error::unauthorized(detail)`, 401, for a credential that stands for no caller, with a `WWW-Authenticate` challenge;
- `Error::new(StatusCode::FORBIDDEN, detail)`, 403, for a caller refused;
- `Error::internal(cause)`, 500, logged, for a failure of the service: a store that is down. A client does not then
  take its valid token for a wrong one.

## Schemes

A scheme is how a credential travels. It is one of three kinds.

**Given by the crate.** `Bearer`, whose credential is the token, a `String`. `Basic`, whose credential is a
`quoll::BasicCredential { user, password }`, whose `Debug` hides the password. `Basic` makes a browser open its own
login window: a route that a web page calls takes another scheme.

**Declared in the contract**, with its visibility and where its credential travels. Each becomes a type of that name,
whose credential is a `String`, and its `///` describes it in the OpenAPI document:

```rust
quoll::api! {
    pub api Api = "/api";

    /// The key of an operator's scripts.
    pub scheme Key = header "X-Api-Key";

    /// The session of the web page.
    pub scheme Session = cookie "__Host-session";

    /// The token of a link sent by mail.
    pub scheme Link = query "token";

    /// A token of the company's provider.
    pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> u64;
    }

    #[auth(Session | Login)]
    pub group Notes {
        GET "/notes" => list() -> Vec<String>;
    }

    #[auth(Link)]
    pub group Shares {
        GET "/shared" => shared() -> String;
    }

    #[auth(Basic)]
    pub group Metrics {
        GET "/metrics" => metrics() -> String;
    }
}
```

- `header`: a header of its own. Given twice, or empty, it is a 401.
- `cookie`: a cookie. Of a cookie given twice, the first value that is not empty is read.
- `query`: a key of the query. Given twice, or empty, it is a 401. A route that takes it reads no other value of that
  key: the macro refuses it in the route's string, but cannot see the fields of a query struct, none of which is to
  be named so.
- `openid`: a bearer token from an OpenID Connect provider, which the document describes so. Quoll checks no token:
  `authenticate` does.

**The application's own**: a type implementing `quoll::Scheme`, as an HMAC signature, named in `#[auth(...)]` by its
path, `#[auth(crate::auth::Hmac)]`. Its `read` gives the credential from the request's head, `None` when the request
carries none, and an error when it carries one that is not well formed. The doc of `quoll::Scheme` shows one.

## Alternatives

`#[auth(Bearer | Key)]` takes either scheme, up to four. Both give the same type of caller: the service implements
`Authenticate<Bearer>` and `Authenticate<Key>` with one `Caller`.

- The schemes are tried in the order written: the first whose credential the request carries decides.
- A wrong credential of that scheme is refused with its error, and the next scheme is not tried.
- A request may carry two credentials, a session cookie beside a token: neither is refused for the other.
- A 401 carries one `WWW-Authenticate` per scheme of the route, in that order.

## Optional

`#[auth(Bearer, optional)]` gives the handler `None` for a request with no credential of any of its schemes. A
credential that is malformed, or that `authenticate` refuses, is still answered 401.

```rust
use quoll::{Authenticate, Bearer, Error, Response, Result};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, optional)]
        GET "/notes/{id}/likes" => likes(id: i64) -> String;
    }
}

struct User {
    name: String,
}

struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        match token.as_str() {
            "ana-token" => Ok(User { name: "ana".into() }),
            _ => Err(Error::unauthorized("unknown or expired token")),
        }
    }
}

impl NotesServer for App {
    async fn likes(&self, user: Option<&User>, id: i64) -> Result<Response<String>> {
        let who = user.map_or("someone", |user| user.name.as_str());

        Ok(Response::new(format!("{who} reads the likes of note {id}")))
    }
}
```

## Scopes

`#[auth(Bearer, scopes("repo", "admin"))]` needs every scope it names. The caller implements `quoll::Scoped`, and the
router answers 403 when the caller lacks one: after `authenticate`, before the handler, and before the path, the query
and the body are read.

```rust
use quoll::{Authenticate, Bearer, Error, Response, Result, Scoped};

quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        #[auth(Bearer, scopes("admin"))]
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

/// Who a request comes from, with the scopes its token grants.
struct User {
    scopes: Vec<String>,
}

impl Scoped for User {
    fn has_scope(&self, scope: &str) -> bool {
        self.scopes.iter().any(|granted| granted == scope)
    }
}

struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        match token.as_str() {
            "admin-token" => Ok(User {
                scopes: vec!["admin".into()],
            }),
            _ => Err(Error::unauthorized("unknown or expired token")),
        }
    }
}

impl NotesServer for App {
    async fn remove(&self, _user: &User, _id: i64) -> Result<Response<()>> {
        Ok(Response::new(()))
    }
}
```

`optional` with `scopes` does not compile: a request with no credential has no scope to check. The OpenAPI document
lists each route's scopes.

## Cookie schemes and their origin check

A browser sends a cookie with a request that a page of another site makes it send. So a request that carries the
cookie of a cookie scheme, with a method other than `GET`, `HEAD` or `OPTIONS`, is answered 403 unless it comes from
the server's own origin:

- its `Sec-Fetch-Site` is `same-origin` or `none`;
- or, from a browser that sends no `Sec-Fetch-Site`, its `Origin` is its `Host`.

A request with neither header comes from no browser, and passes. Behind a reverse proxy, the proxy passes the
browser's `Host`, as nginx does with `proxy_set_header Host $host`; `X-Forwarded-Host` is not read.

Only the server's own origin is trusted for now: a page served from another origin calls the api with another scheme,
even with `server::cors`. The application sets its cookie `SameSite=Lax` or `Strict`, `HttpOnly` and `Secure`, and
names it with the prefix `__Host-`, which a subdomain cannot set.

## On the client

Each scheme has its middleware, which sends the credential on the routes that take it, and on no other:

| The route takes                  | The client sends it with      |
| -------------------------------- | ----------------------------- |
| `Bearer`                         | `client::bearer(token)`, or `client::bearer_from(source)` |
| `Basic`                          | `client::basic(user, password)` |
| a declared scheme `Key`          | `client::key::<Key>(value)`   |
| a scheme of the application's    | `client::on_request(f)`, or a middleware of its own |

[The client](client.md#credentials) says how each behaves.
