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
- A service whose group takes `Bearer` implements `Authenticate<Bearer>`. A service that does not, does not compile.
- `authenticate` runs before the handler, and before the request's body is read.
- The contract names no type for the caller: that type lives on the server.

## What a route takes

| On a group or a route              | The handler receives            | Without a valid credential                       |
| ---------------------------------- | ------------------------------- | ------------------------------------------------ |
| `#[auth(Bearer)]`                  | `&Caller`                       | 401                                              |
| `#[auth(Bearer \| Key)]`           | `&Caller`, from either scheme   | 401                                              |
| `#[auth(Bearer, optional)]`        | `Option<&Caller>`               | `None` without a credential, 401 with a wrong one |
| `#[auth(Bearer, scopes("repo"))]`  | `&Caller`                       | 401, or 403 when the caller lacks a scope        |
| `#[auth(none)]`, on a route        | nothing                         | the route leaves its group's authentication     |

A route's attribute replaces the attribute of its group.

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
- `Error::new(StatusCode::FORBIDDEN, detail)`, 403, for a refused caller;
- `Error::internal(cause)`, 500, logged, for a failure of the service, such as a store that is down. Then a client
  does not take its valid token for a wrong one.

## Schemes

A scheme is how a credential travels. It is one of three kinds.

**Given by the crate.** `Bearer`, whose credential is the token, a `String`. `Basic`, whose credential is a
`quoll::BasicCredential { user, password }`; its `Debug` hides the password. `Basic` makes a browser open its own
login window, so a route that a web page calls takes another scheme.

**Declared in the contract**, with its visibility and where its credential travels. Each one becomes a type of that
name, whose credential is a `String`. Its `///` comment describes it in the OpenAPI document:

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
- `cookie`: a cookie. When the cookie is given twice, the first value that is not empty is read.
- `query`: a key of the query. Given twice, or empty, it is a 401. A route that takes it reads no other value of that
  key. The macro refuses the key in the route's string. It cannot see the fields of a query struct: make sure none
  of them has that name.
- `openid`: a bearer token from an OpenID Connect provider, and the document describes it that way. Quoll checks no
  token: `authenticate` does.

**The application's own**, such as an HMAC signature or the format of another API's key. `#[auth(...)]` names it by
its path: `#[auth(crate::auth::Hmac)]`. [A scheme of the application's own](#a-scheme-of-the-applications-own) says
how to write one.

## A scheme of the application's own

A scheme is a type that implements `quoll::Scheme`, which gives its name, and the sides its api uses:

- `quoll::ServerScheme`: how a server reads the credential, the challenge of a 401, and how the OpenAPI document
  describes it;
- `quoll::ClientScheme`: how a client writes the credential on a request.

The api checks that each scheme of `#[auth(...)]` has its sides:

| The api                           | The scheme implements            |
| --------------------------------- | -------------------------------- |
| serves and calls, the default     | `ServerScheme` and `ClientScheme` |
| `#[server(none)]`, it only calls  | `ClientScheme`                   |
| `#[client(none)]`, it only serves | `ServerScheme`                   |
| a `quoll::group!`                 | the sides of the group           |

A scheme without a side its api needs does not compile, whatever the features. The message names the trait to
implement.

### The client side

Jellyfin takes its key as `Authorization: MediaBrowser Token="<key>"`. A client of it writes the header in `write`,
and sends the key with `client::key`:

```rust,no_run
use quoll::auth::{Outgoing, Unsendable};
use quoll::{ClientScheme, Scheme, client};

/// A Jellyfin API key.
pub struct MediaBrowser;

impl Scheme for MediaBrowser {
    const NAME: &'static str = "MediaBrowser";
}

impl ClientScheme for MediaBrowser {
    type Secret = String;

    fn write(key: &String, request: &mut Outgoing<'_>) -> Result<(), Unsendable> {
        request.authorization_params("MediaBrowser", &[("Token", key)])
    }
}

quoll::api! {
    #[server(none)]
    pub api Jellyfin = "";

    #[auth(MediaBrowser)]
    pub group Library {
        GET "/Items/Counts" => counts() -> serde_json::Value;
    }
}

# async fn run() -> Result<(), Box<dyn std::error::Error>> {
let jellyfin = client::builder()
    .with(client::key::<MediaBrowser>("0123abcd"))
    .build::<Jellyfin>("http://jellyfin.local:8096")?;

let counts = jellyfin.library().counts().await?;
# Ok(())
# }
```

`client::key` calls `write` on each request whose route takes the scheme, and on no other. It marks each header
written sensitive, and sends nothing to another origin.

A redirect to another origin gets nothing when the client follows it with `client::redirects`, which
`client::builder()` adds once it is given a credential. A reqwest client that follows redirects itself, as one given
to `Api::client_with` does by default, strips only `Authorization`, `Cookie` and `Proxy-Authorization` on a hop to
another host. Any other header that `write` adds, such as `X-Signature`, goes with it.

`Outgoing` reads the request's `method()`, `path()`, `query()` and `body()`, as a signature needs them. It writes the
credential with one of these, each of which checks the value as a server would read it:

| Method                                   | Writes                                 |
| ---------------------------------------- | -------------------------------------- |
| `authorization_token68(scheme, token)`   | `Authorization: token 65eaa9c8`        |
| `authorization_params(scheme, &[(name, value)])` | `Authorization: MediaBrowser Token="0123abcd"` |
| `header(name, value)`                    | any header, `Authorization` with a raw token included |
| `cookie(name, value)`                    | a cookie, after the request's others   |
| `query_pair(key, value)`                 | a key of the query                     |

`authorization_params` writes each value as a quoted-string, with `"` and `\` escaped. A value the server would not
read, an empty one or one with a line break, is an `Unsendable`. The call then fails before it is sent, and its error
never shows the credential.

`Secret` is what `key` is given. It may differ from what the server reads: a client that signs each request is given
its key, and writes the signature of the request.

A test checks what `write` writes with `Outgoing::new`, without a server:

```rust
# use quoll::auth::{Outgoing, Unsendable};
# use quoll::{ClientScheme, Scheme};
# pub struct MediaBrowser;
# impl Scheme for MediaBrowser {
#     const NAME: &'static str = "MediaBrowser";
# }
# impl ClientScheme for MediaBrowser {
#     type Secret = String;
#     fn write(key: &String, request: &mut Outgoing<'_>) -> Result<(), Unsendable> {
#         request.authorization_params("MediaBrowser", &[("Token", key)])
#     }
# }
use http::Method;
use http::header::AUTHORIZATION;

let mut request = Outgoing::new(&Method::GET, "/Items/Counts", None, None);
MediaBrowser::write(&"0123abcd".to_owned(), &mut request)?;

assert_eq!(
    request.added_headers()[AUTHORIZATION],
    r#"MediaBrowser Token="0123abcd""#
);
# Ok::<(), Unsendable>(())
```

### The server side

`read` gives the credential from the request's head. It gives `None` when the request carries none, and a 401 when it
carries one that is not well formed. `quoll::auth` reads the grammar of `Authorization`, so that a scheme does not
parse it by hand:

- `auth::authorization(&request, "MediaBrowser")` gives the credentials of that auth-scheme, compared without case. It
  gives `None` for a request with none of them, and a 401 when another `Authorization` is beside them.
- `Credentials::token68()` gives a token, as in `token 65eaa9c8`.
- `Credentials::param("Token")` gives the value of a param, as in `MediaBrowser Token="0123abcd"`. Its name is
  compared without case, and the other params are ignored. A list that is not well formed, or with the param twice,
  is a 401.
- `Credentials::as_bytes()` gives the rest, as it is.

```rust
use http::request::Parts;
use quoll::openapi::SecurityScheme;
use quoll::{Result, Scheme, ServerScheme, auth};

/// A Jellyfin API key.
pub struct MediaBrowser;

impl Scheme for MediaBrowser {
    const NAME: &'static str = "MediaBrowser";
}

impl ServerScheme for MediaBrowser {
    type Credential = String;

    fn read(request: &Parts) -> Result<Option<String>> {
        let Some(credentials) = auth::authorization(request, "MediaBrowser")? else {
            return Ok(None);
        };

        credentials.param("Token")
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::http("MediaBrowser")
    }
}

quoll::api! {
    #[client(none)]
    pub api Media = "";

    #[auth(MediaBrowser)]
    pub group Library {
        GET "/Items/Counts" => counts() -> u64;
    }
}
```

A param that is absent is `None`, so the route answers 401. The doc of `quoll::ServerScheme` shows a signature read
from a header of its own.

quoll does not check where a scheme of the application's travels. Two schemes of an alternative that read one header,
or an argument that reads the credential's header, are the application's to avoid.

## Alternatives

`#[auth(Bearer | Key)]` takes either scheme, and a route may name up to four. Both give the same type of caller: the
service implements `Authenticate<Bearer>` and `Authenticate<Key>` with one `Caller`.

- The schemes are tried in the order written: the first whose credential the request carries decides.
- A wrong credential of that scheme is refused with its error, and the next scheme is not tried.
- A request may carry two credentials, such as a session cookie and a token. Neither is refused because of the other.
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

`#[auth(Bearer, scopes("repo", "admin"))]` needs every scope it names. The caller implements `quoll::Scoped`. The
router answers 403 when the caller lacks a scope. It checks after `authenticate`, before the handler, and before it
reads the path, the query and the body.

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

A browser sends a cookie with a request that a page of another site makes it send. So the server answers 403 to a
request that carries the cookie of a cookie scheme, with a method other than `GET`, `HEAD` or `OPTIONS`, unless the
request comes from the server's own origin:

- its `Sec-Fetch-Site` is `same-origin` or `none`;
- or, from a browser that sends no `Sec-Fetch-Site`, its `Origin` is its `Host`.

A request with neither header comes from no browser, and passes. Behind a reverse proxy, the proxy passes on the
browser's `Host`: nginx does it with `proxy_set_header Host $host`. `X-Forwarded-Host` is not read.

For now, only the server's own origin is trusted. A page served from another origin calls the api with another scheme,
even with `server::cors`. The application sets its cookie `SameSite=Lax` or `Strict`, `HttpOnly` and `Secure`. It
names the cookie with the prefix `__Host-`, which a subdomain cannot set.

## On the client

Each scheme has its middleware, which sends the credential on the routes that take it, and on no other:

| The route takes                  | The client sends it with      |
| -------------------------------- | ----------------------------- |
| `Bearer`                         | `client::bearer(token)`, or `client::bearer_from(source)` |
| `Basic`                          | `client::basic(user, password)` |
| a declared scheme `Key`          | `client::key::<Key>(value)`   |
| an application's own scheme `S`  | `client::key::<S>(secret)`    |

[The client](client.md#credentials) says how each behaves.
