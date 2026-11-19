//! Middleware for the client a contract becomes: reqwest-middleware's, which a `ClientWithMiddleware` given to
//! `Api::client_with` runs on each request.

use std::future::Future;
use std::pin::Pin;

use http::{Extensions, HeaderValue, header::AUTHORIZATION};
use reqwest::{Request, Response};
use reqwest_middleware::{Middleware, Next};

use crate::__private::Given;
use crate::rest::auth::is_b64token;
use crate::rest::{Bearer, Route};

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
    let token = token.into();
    let header = is_b64token(token.as_bytes())
        .then(|| HeaderValue::try_from(format!("Bearer {token}")).ok())
        .flatten()
        .map(|mut value| {
            value.set_sensitive(true);
            value
        });

    middleware(move |mut request, extensions, next| {
        if !takes(extensions, <Bearer as Given>::NAME) {
            return next.run(request, extensions);
        }

        let Some(value) = &header else {
            return Box::pin(async { Err(reqwest_middleware::Error::middleware(InvalidToken)) });
        };

        request.headers_mut().insert(AUTHORIZATION, value.clone());
        next.run(request, extensions)
    })
}

/// Whether the route of a request takes the scheme `name`. A request without a route takes none.
fn takes(extensions: &Extensions, name: &str) -> bool {
    extensions
        .get::<Route>()
        .is_some_and(|route| route.schemes().contains(&name))
}

/// What a middleware's call returns: the answer of the rest of the chain.
type Handled<'a> = Pin<Box<dyn Future<Output = reqwest_middleware::Result<Response>> + Send + 'a>>;

/// Gives a closure the signature reqwest-middleware takes, for every lifetime of its request.
fn middleware<F>(handle: F) -> F
where
    F: for<'a> Fn(Request, &'a mut Extensions, Next<'a>) -> Handled<'a> + Send + Sync + 'static,
{
    handle
}

/// A bearer token that is not a `b64token`. It does not show the token.
#[derive(Debug)]
struct InvalidToken;

impl std::fmt::Display for InvalidToken {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("the bearer token cannot be sent: it is not a `b64token`, letters, digits, `-._~+/` and `=` at its end")
    }
}

impl std::error::Error for InvalidToken {}
