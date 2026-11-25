//! The client of a contract: [`builder()`], which builds it with middleware, and the usual middleware, on
//! reqwest-middleware; and [`follow()`], which calls a stream route again each time its stream ends.
//!
//! A client runs its middleware on each request in the order they are added: the first sees the request first, and
//! the answer last.
//!
//! | Middleware        | What it does                                                                        |
//! | ----------------- | ----------------------------------------------------------------------------------- |
//! | [`bearer()`]      | `Authorization: Bearer`, on the routes that take [`Bearer`]                         |
//! | [`bearer_from()`] | the same, with a token asked of a [`TokenSource`] before each call, and after a 401 |
//! | [`basic()`]       | `Authorization: Basic`, on the routes that take [`Basic`]                           |
//! | [`key()`]         | the credential of a scheme the contract declares, where it travels                  |
//! | [`on_request()`]  | a closure that changes each request of a contract before it leaves                  |
//! | [`user_agent()`]  | `User-Agent`, on a request that has none                                            |
//! | [`request_id()`]  | a new `X-Request-Id` per call, on a request that has none                           |
//! | [`retry()`]       | a call tried again after a failure that may pass, or as a closure decides           |
//! | [`timeout()`]     | a deadline per attempt and per hop                                                  |
//! | [`trace()`]       | a tracing span per call, named by its operation                                     |
//! | [`redirects()`]   | redirects followed one hop at a time, through the middleware after it               |
//!
//! The [`Builder`] puts the helpers of quoll in the order they work in, whatever the order they are given in, and any
//! other middleware where it is given:
//!
//! ```no_run
//! # use std::time::Duration;
//! use quoll::client;
//! # quoll::api! {
//! #     pub api Api = "/api";
//! #     #[auth(Bearer)]
//! #     pub group Notes { GET "/notes/{id}" => get(id: i64) -> String; }
//! # }
//!
//! # async fn run() -> Result<(), Box<dyn std::error::Error>> {
//! let api = client::builder()
//!     .with(client::bearer("mF_9.B5f-4.1JqM"))
//!     .with(client::redirects(5))
//!     .with(client::retry(3))
//!     .with(client::request_id())
//!     .with(client::timeout(Duration::from_secs(10)))
//!     .with(client::user_agent("notes-cli/0.1"))
//!     .with(client::trace())
//!     .build::<Api>("http://localhost:8080")?;
//! # Ok(())
//! # }
//! ```
//!
//! The helpers of authentication, [`bearer`], [`bearer_from`], [`basic`] and [`key`], and the closure of
//! [`on_request`], act on the requests of a contract to its api's own origin only: a request that carries no
//! [`Route`], as one sent to another host through the same `ClientWithMiddleware`, and a hop of [`redirects`] to
//! another origin, go as they are. [`redirects`] comes before all of them, so that they run on each hop.
//!
//! Each helper is a middleware for `reqwest_middleware::ClientBuilder` too, whose `ClientWithMiddleware`
//! `Api::client_with` takes. There, they run in the order they are added, which is to be the one the builder puts
//! them in, from the first to the last:
//!
//! ```no_run
//! # use std::time::Duration;
//! # use quoll::client;
//! # quoll::api! {
//! #     pub api Api = "/api";
//! #     #[auth(Bearer)]
//! #     pub group Notes { GET "/notes/{id}" => get(id: i64) -> String; }
//! # }
//! # async fn run() -> Result<(), Box<dyn std::error::Error>> {
//! let http = reqwest::Client::builder()
//!     .redirect(reqwest::redirect::Policy::none())
//!     .build()?;
//! let http = reqwest_middleware::ClientBuilder::new(http)
//!     .with(client::trace()) // one span around the whole call
//!     .with(client::user_agent("notes-cli/0.1"))
//!     .with(client::request_id()) // before `retry`: one id for every attempt
//!     .with(client::retry(3))
//!     .with(client::timeout(Duration::from_secs(10))) // per attempt and per hop
//!     .with(client::redirects(5)) // before every credential: each hop is given it, or not
//!     .with(client::bearer("mF_9.B5f-4.1JqM"))
//!     .build();
//! let api = Api::client_with(http, "http://localhost:8080")?;
//! # Ok(())
//! # }
//! ```

use std::borrow::Cow;
use std::future::Future;
use std::pin::Pin;

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use http::header::GetAll;
use http::header::{AUTHORIZATION, COOKIE};
use http::{Extensions, HeaderName, HeaderValue};
use reqwest::{Request, Response};
use reqwest_middleware::{Middleware, Next};

use crate::__private::auth::{Declared, Place};
use crate::__private::client::ApiOrigin;
use crate::auth::is_b64token;
use crate::{Basic, Bearer, Route, Scheme};

mod builder;
mod follow;
mod redirects;
mod request;
mod retry;
mod token;
mod trace;

pub use builder::{Builder, Contract, builder};
pub use follow::{Followed, Following, Reconnect, follow};
pub use redirects::redirects;
pub(crate) use request::{CallTimeout, HeadTimedOut, KeepsDeadline, calls_timeout};
pub use request::{on_request, request_id, timeout, user_agent};
pub use retry::{Attempt, Retry, Retrying, retry};
pub use token::{TokenSource, bearer_from};
pub use trace::trace;

/// Sends `Authorization: Bearer <token>` on the requests whose [`Route`] takes [`Bearer`].
///
/// It sends it to the api's own origin, and on no other request: a route with `#[auth(none)]`, a request that did not
/// come from the client of a contract and carries no `Route`, as one sent to another host through the same
/// `ClientWithMiddleware`, or a hop of [`redirects`] to another origin, never gets the token. A token that expires
/// comes from a source, with [`bearer_from`].
///
/// The header is marked sensitive, so that a log of the request does not show it. The token is an RFC 6750
/// `b64token`, letters, digits, `-._~+/` and `=` at its end, which is all a server of a contract reads: any other, as
/// one with a space or a line break, fails each call that would send it, with an error that does not show it, rather
/// than a 401 that would look like an expired token.
///
/// ```no_run
/// # use quoll::client;
/// # quoll::api! {
/// #     pub api Api = "/api";
/// #     #[auth(Bearer)]
/// #     pub group Notes { GET "/notes/{id}" => get(id: i64) -> String; }
/// # }
/// # async fn run() -> quoll::Result<()> {
/// let api = client::builder()
///     .with(client::bearer("mF_9.B5f-4.1JqM"))
///     .build::<Api>("http://localhost:8080")?;
///
/// let note = api.notes().get(1).await?;
/// # Ok(())
/// # }
/// ```
pub fn bearer(token: impl Into<String>) -> impl Middleware {
    let header = bearer_header(&token.into());

    send::<Bearer>(authorization(
        header,
        "the bearer token cannot be sent: it is not a `b64token`, letters, digits, `-._~+/` and `=` at its end",
    ))
}

/// Sends `Authorization: Basic <base64 of user:password>` on the requests whose [`Route`] takes [`Basic`], and on no
/// other, as [`bearer`] sends its token.
///
/// The header is marked sensitive. A user with a `:`, or a user or a password with a control character, which the
/// server would not read, fails each call that would send them, with an error that shows neither.
///
/// ```no_run
/// # use quoll::client;
/// # quoll::api! {
/// #     pub api Api = "/api";
/// #     #[auth(Basic)]
/// #     pub group Admin { GET "/admin/stats" => stats() -> String; }
/// # }
/// # async fn run() -> quoll::Result<()> {
/// let api = client::builder()
///     .with(client::basic("ana", "secret"))
///     .build::<Api>("http://localhost:8080")?;
///
/// let stats = api.admin().stats().await?;
/// # Ok(())
/// # }
/// ```
pub fn basic(user: impl Into<String>, password: impl Into<String>) -> impl Middleware {
    let (user, password) = (user.into(), password.into());
    let readable = !user.contains(':')
        && !user.chars().any(char::is_control)
        && !password.chars().any(char::is_control);
    let header = readable
        .then(|| {
            let encoded = STANDARD.encode(format!("{user}:{password}"));

            HeaderValue::try_from(format!("Basic {encoded}")).ok()
        })
        .flatten();

    send::<Basic>(authorization(
        sensitive(header),
        "the basic credential cannot be sent: its user holds a `:`, or its user or its password a control character",
    ))
}

/// Sends `value`, the credential of the scheme `K` that the contract declares, where `K` says it travels.
///
/// It sends it on the requests whose [`Route`] takes `K`, and on no other, as [`bearer`] sends its token:
///
/// - `scheme Key = header "X-Api-Key";`: in the header, marked sensitive;
/// - `scheme Session = cookie "session";`: as the cookie `session`, after the cookies the request already has, all in
///   one `Cookie` header; reqwest adds the cookies of its jar only to a request with no `Cookie` header, so none of
///   them is sent to a route that takes the scheme;
/// - `scheme Token = query "token";`: as the key `token` of the query, after the route's own, which a log of the URL
///   shows;
/// - `scheme Login = openid "...";`: in `Authorization: Bearer`.
///
/// A route is told by the scheme's type, not its name: two contracts that each declare a `Key`, called through one
/// `ClientWithMiddleware`, each get their own. A value the server would not read fails each call that would send it,
/// with an error that does not show it: an empty one; in a header, one with a control character, or a space or a tab
/// at either end; in a cookie, one with a character other than an ASCII letter, digit or sign, or with `"`, `,`, `;`
/// or `\`; for OpenID Connect, a token that is not a `b64token`.
///
/// The key of a header scheme is a header of its own, which reqwest, as curl and `fetch`, keeps on a redirect it
/// follows to another host: it strips only `Authorization`, `Cookie` and `Proxy-Authorization` there. [`redirects`]
/// is the way to follow redirects without the key: on a reqwest client that follows none, it follows each hop itself,
/// through the middleware after it, and `key` sends the key to the api's own origin only.
///
/// ```no_run
/// # use quoll::client;
/// quoll::api! {
///     pub api Api = "/api";
///
///     /// The key of an operator's scripts.
///     pub scheme Key = header "X-Api-Key";
///
///     #[auth(Key)]
///     pub group Admin {
///         GET "/admin/stats" => stats() -> String;
///     }
/// }
///
/// # async fn run() -> Result<(), Box<dyn std::error::Error>> {
/// let api = client::builder()
///     .with(client::redirects(5))
///     .with(client::key::<Key>("k-1"))
///     .build::<Api>("http://localhost:8080")?;
///
/// let stats = api.admin().stats().await?;
/// # Ok(())
/// # }
/// ```
pub fn key<K: Declared>(value: impl Into<String>) -> impl Middleware {
    let value = value.into();

    match K::PLACE {
        Place::Header(name) => {
            let name = HeaderName::from_static(name);
            let header = header_value(&value);

            send::<K>(Box::new(move |request| {
                let value = header.clone().ok_or(Failure::new(
                    "the key cannot be sent in its header: it is empty, or holds a control character or a space at either end",
                ))?;

                request.headers_mut().insert(name.clone(), value);
                Ok(())
            }))
        }
        Place::Cookie(name) => {
            let pair = is_cookie_value(&value).then(|| format!("{name}={value}"));

            send::<K>(Box::new(move |request| {
                add_cookie(request, pair.as_deref())
            }))
        }
        Place::Query(name) => {
            let value = (!value.is_empty()).then_some(value);

            send::<K>(Box::new(move |request| {
                let value = value.as_deref().ok_or(Failure::new(
                    "the key cannot be sent in the query: it is empty",
                ))?;

                set_query_pair(request, name, value);
                Ok(())
            }))
        }
        Place::Bearer => send::<K>(authorization(
            bearer_header(&value),
            "the OpenID Connect token cannot be sent: it is not a `b64token`, letters, digits, `-._~+/` and `=` at its end",
        )),
    }
}

/// Puts `name=value` at the end of the request's query, in the place of any value of `name` it has: a call of a
/// route has none, as its query never holds the key, but the URL of a redirect may.
fn set_query_pair(request: &mut Request, name: &str, value: &str) {
    let url = request.url_mut();

    if url.query_pairs().any(|(key, _)| key == name) {
        let kept: Vec<(String, String)> = url
            .query_pairs()
            .filter(|(key, _)| key != name)
            .map(|(key, value)| (key.into_owned(), value.into_owned()))
            .collect();

        url.query_pairs_mut().clear().extend_pairs(kept);
    }

    url.query_pairs_mut().append_pair(name, value);
}

/// Adds `pair`, `name=value`, to the request's cookies, in one `Cookie` header that holds every cookie the request had
/// before, byte for byte, then the pair.
fn add_cookie(request: &mut Request, pair: Option<&str>) -> Result<(), Failure> {
    let pair = pair.ok_or(Failure::new(
        "the key cannot be sent as a cookie: it is empty, or holds a character a cookie's value cannot",
    ))?;

    let cookies = merged(request.headers().get_all(COOKIE), pair);

    request.headers_mut().insert(COOKIE, cookies);
    Ok(())
}

/// The cookies of `headers`, each header's value as it is, then `pair`, joined with `; ` as one `Cookie` header, marked
/// sensitive.
fn merged(headers: GetAll<'_, HeaderValue>, pair: &str) -> HeaderValue {
    let mut cookies: Vec<u8> = Vec::new();

    for value in headers {
        let value = value.as_bytes().trim_ascii();

        if !value.is_empty() {
            cookies.extend_from_slice(value);
            cookies.extend_from_slice(b"; ");
        }
    }
    cookies.extend_from_slice(pair.as_bytes());

    let mut value = HeaderValue::from_bytes(&cookies).expect(
        "header values and a cookie pair, which hold no control character, make a header value",
    );
    value.set_sensitive(true);
    value
}

/// `Bearer <token>`, marked sensitive, or `None` for a token that is not a `b64token`, which no server of a contract
/// reads.
fn bearer_header(token: &str) -> Option<HeaderValue> {
    let value = is_b64token(token.as_bytes())
        .then(|| HeaderValue::try_from(format!("Bearer {token}")).ok())
        .flatten();

    sensitive(value)
}

/// A header's value, marked sensitive, or `None` for an empty one, or one with a control character or a space or a
/// tab at either end, which HTTP would drop.
fn header_value(value: &str) -> Option<HeaderValue> {
    let blank = |c: char| c == ' ' || c == '\t';
    let sendable = !value.is_empty()
        && !value.starts_with(blank)
        && !value.ends_with(blank)
        && !value.chars().any(|c| c.is_control() && c != '\t');

    sensitive(
        sendable
            .then(|| HeaderValue::try_from(value).ok())
            .flatten(),
    )
}

/// Whether `value` is an RFC 6265 `cookie-value` with no quotes: ASCII letters, digits and signs, but `"`, `,`, `;`
/// and `\`.
fn is_cookie_value(value: &str) -> bool {
    !value.is_empty()
        && value
            .bytes()
            .all(|byte| byte.is_ascii_graphic() && !b"\",;\\".contains(&byte))
}

/// Marks a header's value sensitive, so that a log of the request does not show it.
fn sensitive(value: Option<HeaderValue>) -> Option<HeaderValue> {
    value.map(|mut value| {
        value.set_sensitive(true);
        value
    })
}

/// Puts `header` in `Authorization`, or fails the request with `unsendable` when there is no header to send.
fn authorization(header: Option<HeaderValue>, unsendable: &'static str) -> Box<Add> {
    Box::new(move |request| {
        let value = header.clone().ok_or(Failure::new(unsendable))?;

        request.headers_mut().insert(AUTHORIZATION, value);
        Ok(())
    })
}

/// A middleware that calls `add` on the requests whose route takes `S`, to the api's own origin, and leaves the others
/// as they are: a route with `#[auth(none)]`, a request that did not come from the client of a contract and carries
/// no `Route`, or a hop of [`redirects`] to another origin, never gets the credential.
fn send<S: Scheme>(add: Box<Add>) -> impl Middleware {
    helper(Rank::Credential, move |mut request, extensions, next| {
        if !own_route(&request, extensions).is_some_and(Route::takes::<S>) {
            return next.run(request, extensions);
        }

        if let Err(failure) = add(&mut request) {
            return Box::pin(async move { Err(reqwest_middleware::Error::middleware(failure)) });
        }

        next.run(request, extensions)
    })
}

/// The [`Route`] of a request of a contract to its api's own origin, or `None` for any other request: one that did not
/// come from the client of a contract, or a hop of [`redirects`] to another origin.
fn own_route<'a>(request: &Request, extensions: &'a Extensions) -> Option<&'a Route> {
    let origin = extensions.get::<ApiOrigin>()?;

    if origin.0 != request.url().origin() {
        return None;
    }

    extensions.get::<Route>()
}

/// What a middleware does to a request whose route takes its scheme: one type for every place a credential travels.
type Add = dyn Fn(&mut Request) -> Result<(), Failure> + Send + Sync;

/// What a middleware's call returns: the answer of the rest of the chain.
type Handled<'a> = Pin<Box<dyn Future<Output = reqwest_middleware::Result<Response>> + Send + 'a>>;

/// A helper of quoll's, ranked: a closure given the signature reqwest-middleware takes, for every lifetime of its
/// request.
fn helper<F>(rank: Rank, handle: F) -> Helper
where
    F: for<'a> Fn(Request, &'a mut Extensions, Next<'a>) -> Handled<'a> + Send + Sync + 'static,
{
    Helper::new(rank, handle)
}

/// Where a helper of quoll's goes among the others a [`Builder`] is given, from the first, which sees the request
/// first, to the last: the order the helpers work in.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Rank {
    /// [`trace`]: one span around the whole call.
    Trace,
    /// [`user_agent`].
    UserAgent,
    /// [`request_id`]: before `retry`, one id for every attempt of a call.
    RequestId,
    /// [`retry`].
    Retry,
    /// [`timeout`]: a deadline per attempt and per hop.
    Timeout,
    /// [`redirects`]: before every credential, so that each hop is given it, or not.
    Redirects,
    /// [`bearer`], [`bearer_from`], [`basic`], [`key`] and [`on_request`], in the order they are given.
    Credential,
}

/// What each helper of quoll's returns, behind an `impl Middleware`: its middleware, and its rank, by which a
/// [`Builder`] tells it from a middleware of the application's.
pub(crate) struct Helper {
    pub(crate) rank: Rank,
    inner: Box<dyn Middleware>,
}

impl Helper {
    fn new(rank: Rank, inner: impl Middleware) -> Self {
        Self {
            rank,
            inner: Box::new(inner),
        }
    }
}

/// The signature reqwest-middleware's `Middleware` has, as the `async-trait` it is written with spells it out: the
/// future of the middleware inside is passed on as it is, with no box of its own.
impl Middleware for Helper {
    fn handle<'life0, 'life1, 'life2, 'async_trait>(
        &'life0 self,
        request: Request,
        extensions: &'life1 mut Extensions,
        next: Next<'life2>,
    ) -> Pin<Box<dyn Future<Output = reqwest_middleware::Result<Response>> + Send + 'async_trait>>
    where
        'life0: 'async_trait,
        'life1: 'async_trait,
        'life2: 'async_trait,
        Self: 'async_trait,
    {
        self.inner.handle(request, extensions, next)
    }
}

/// Why a middleware fails a call: a credential that cannot be sent as it is, whose message does not show it, or a
/// redirect not followed.
#[derive(Debug)]
struct Failure(Cow<'static, str>);

impl Failure {
    fn new(message: impl Into<Cow<'static, str>>) -> Self {
        Self(message.into())
    }

    /// The error of reqwest-middleware that fails the call with `message`.
    fn fail(message: impl Into<Cow<'static, str>>) -> reqwest_middleware::Error {
        reqwest_middleware::Error::middleware(Self::new(message))
    }
}

impl std::fmt::Display for Failure {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Failure {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_cookie_joins_every_cookie_the_request_had_in_one_header() {
        let mut headers = http::HeaderMap::new();
        headers.append(COOKIE, HeaderValue::from_static("theme=dark"));
        headers.append(COOKIE, HeaderValue::from_static("lang=fr; tz=utc"));
        headers.append(
            COOKIE,
            HeaderValue::from_bytes(b"name=caf\xe9").expect("a header value"),
        );

        let value = merged(headers.get_all(COOKIE), "session=s-1");

        assert_eq!(
            value.as_bytes(),
            b"theme=dark; lang=fr; tz=utc; name=caf\xe9; session=s-1"
        );
        assert!(value.is_sensitive());
    }

    #[test]
    fn a_query_key_takes_the_place_of_one_a_redirect_kept() {
        let url = reqwest::Url::parse("http://localhost/api/token?page=2&token=old&q=a+b")
            .expect("a url");
        let mut request = Request::new(http::Method::GET, url);

        set_query_pair(&mut request, "token", "new");
        assert_eq!(request.url().query(), Some("page=2&q=a+b&token=new"));

        let url = reqwest::Url::parse("http://localhost/api/token?q=a%20b").expect("a url");
        let mut request = Request::new(http::Method::GET, url);

        set_query_pair(&mut request, "token", "new");
        assert_eq!(request.url().query(), Some("q=a%20b&token=new"));
    }

    #[test]
    fn a_cookie_alone_is_the_header() {
        let headers = http::HeaderMap::new();

        assert_eq!(
            merged(headers.get_all(COOKIE), "session=s-1"),
            "session=s-1"
        );
    }
}
