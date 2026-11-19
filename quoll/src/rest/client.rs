//! Middleware for the client a contract becomes: reqwest-middleware's, which a `ClientWithMiddleware` given to
//! `Api::client_with` runs on each request.

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
use crate::rest::auth::is_b64token;
use crate::rest::{Basic, Bearer, Route, Scheme};

/// Sends `Authorization: Bearer <token>` on the requests whose [`Route`] takes [`Bearer`], and on no other: a route
/// with `#[auth(none)]`, or a request that did not come from the client of a contract and carries no `Route`, as one
/// sent to another host through the same `ClientWithMiddleware`, never gets the token.
///
/// The header is marked sensitive, so that a log of the request does not show it. The token is an RFC 6750
/// `b64token`, letters, digits, `-._~+/` and `=` at its end, which is all a server of a contract reads: any other, as
/// one with a space or a line break, fails each call that would send it, with an error that does not show it, rather
/// than a 401 that would look like an expired token.
///
/// ```no_run
/// # use quoll::rest;
/// # rest::api! {
/// #     pub api Api = "/api";
/// #     #[auth(Bearer)]
/// #     pub group Notes { GET "/notes/{id}" => get(id: i64) -> String; }
/// # }
/// # async fn run() -> rest::Result<()> {
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::bearer("mF_9.B5f-4.1JqM"))
///     .build();
/// let api = Api::client_with(http, "http://localhost:8080")?;
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
/// # use quoll::rest;
/// # rest::api! {
/// #     pub api Api = "/api";
/// #     #[auth(Basic)]
/// #     pub group Admin { GET "/admin/stats" => stats() -> String; }
/// # }
/// # async fn run() -> rest::Result<()> {
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::basic("ana", "secret"))
///     .build();
/// let api = Api::client_with(http, "http://localhost:8080")?;
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

/// Sends `value`, the credential of the scheme `K` that the contract declares, where `K` says it travels, on the
/// requests whose [`Route`] takes `K`, and on no other, as [`bearer`] sends its token:
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
/// The key of a header scheme is a header of its own, which reqwest, as curl and `fetch`, keeps on a redirect to
/// another host: it strips only `Authorization`, `Cookie` and `Proxy-Authorization` there. A client that sends one
/// follows no redirect to another origin, with a policy such as this one:
///
/// ```no_run
/// # use quoll::rest;
/// rest::api! {
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
/// // Follows a redirect to the origin, scheme, host and port, of the request that got it only.
/// let same_origin = reqwest::redirect::Policy::custom(|attempt| {
///     let previous = attempt.previous().last();
///     let same = previous.is_some_and(|previous| previous.origin() == attempt.url().origin());
///
///     if same && attempt.previous().len() <= 10 {
///         attempt.follow()
///     } else {
///         attempt.stop()
///     }
/// });
/// let http = reqwest::Client::builder().redirect(same_origin).build()?;
/// let http = reqwest_middleware::ClientBuilder::new(http)
///     .with(rest::client::key::<Key>("k-1"))
///     .build();
/// let api = Api::client_with(http, "http://localhost:8080")?;
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
                let value = header.clone().ok_or(Unsendable(
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
                let value = value.as_deref().ok_or(Unsendable(
                    "the key cannot be sent in the query: it is empty",
                ))?;

                request.url_mut().query_pairs_mut().append_pair(name, value);
                Ok(())
            }))
        }
        Place::Bearer => send::<K>(authorization(
            bearer_header(&value),
            "the OpenID Connect token cannot be sent: it is not a `b64token`, letters, digits, `-._~+/` and `=` at its end",
        )),
    }
}

/// Adds `pair`, `name=value`, to the request's cookies, in one `Cookie` header that holds every cookie the request had
/// before, byte for byte, then the pair.
fn add_cookie(request: &mut Request, pair: Option<&str>) -> Result<(), Unsendable> {
    let pair = pair.ok_or(Unsendable(
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
        let value = header.clone().ok_or(Unsendable(unsendable))?;

        request.headers_mut().insert(AUTHORIZATION, value);
        Ok(())
    })
}

/// A middleware that calls `add` on the requests whose route takes `S`, and leaves the others as they are: a route
/// with `#[auth(none)]`, or a request that did not come from the client of a contract and carries no `Route`, as one
/// sent to another host through the same `ClientWithMiddleware`, never gets the credential.
fn send<S: Scheme>(add: Box<Add>) -> impl Middleware {
    middleware(move |mut request, extensions, next| {
        if !extensions.get::<Route>().is_some_and(Route::takes::<S>) {
            return next.run(request, extensions);
        }

        if let Err(unsendable) = add(&mut request) {
            return Box::pin(async move { Err(reqwest_middleware::Error::middleware(unsendable)) });
        }

        next.run(request, extensions)
    })
}

/// What a middleware does to a request whose route takes its scheme: one type for every place a credential travels.
type Add = dyn Fn(&mut Request) -> Result<(), Unsendable> + Send + Sync;

/// What a middleware's call returns: the answer of the rest of the chain.
type Handled<'a> = Pin<Box<dyn Future<Output = reqwest_middleware::Result<Response>> + Send + 'a>>;

/// Gives a closure the signature reqwest-middleware takes, for every lifetime of its request.
fn middleware<F>(handle: F) -> F
where
    F: for<'a> Fn(Request, &'a mut Extensions, Next<'a>) -> Handled<'a> + Send + Sync + 'static,
{
    handle
}

/// A credential that cannot be sent as it is. Its message does not show the credential.
#[derive(Debug)]
struct Unsendable(&'static str);

impl std::fmt::Display for Unsendable {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for Unsendable {}

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
    fn a_cookie_alone_is_the_header() {
        let headers = http::HeaderMap::new();

        assert_eq!(
            merged(headers.get_all(COOKIE), "session=s-1"),
            "session=s-1"
        );
    }
}
