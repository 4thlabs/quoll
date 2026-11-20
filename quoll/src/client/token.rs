//! A bearer token asked of a source before each call, for tokens that expire.

use std::future::Future;
use std::sync::Arc;

use http::StatusCode;
use http::header::AUTHORIZATION;
use reqwest::Request;
use reqwest_middleware::Middleware;

use super::{Failure, Rank, bearer_header, helper, own_route};
use crate::rest::{Bearer, Result, Route};

/// Where [`bearer_from`] takes its tokens: asked for one before each call, and for a new one after a 401.
///
/// Its methods are written as `async fn`. Two calls that get a 401 at once each ask [`refresh`](Self::refresh), with
/// the token each of them sent: a source that keeps one token refreshes it once, under a lock, and gives the other
/// call the token it has when the one refused is no longer its own.
///
/// ```no_run
/// use quoll::client::{self, TokenSource};
/// use quoll::rest;
/// use tokio::sync::Mutex;
///
/// /// The token of a session, which a call to the authentication server renews.
/// struct Session {
///     token: Mutex<String>,
/// }
///
/// impl TokenSource for Session {
///     async fn token(&self) -> rest::Result<String> {
///         Ok(self.token.lock().await.clone())
///     }
///
///     async fn refresh(&self, refused: &str) -> rest::Result<String> {
///         let mut token = self.token.lock().await;
///
///         // Another call has renewed it since this one sent `refused`.
///         if *token != refused {
///             return Ok(token.clone());
///         }
///
///         *token = renew().await?;
///         Ok(token.clone())
///     }
/// }
///
/// # async fn renew() -> rest::Result<String> { Ok(String::new()) }
/// let http = client::builder()
///     .with(client::bearer_from(Session { token: Mutex::new("t-1".into()) }))
///     .build_http()?;
/// # Ok::<(), quoll::rest::Error>(())
/// ```
pub trait TokenSource: Send + Sync + 'static {
    /// The token to send on a call.
    ///
    /// # Errors
    ///
    /// When there is no token to send: the call fails with this error as the source of its own, and is not sent.
    fn token(&self) -> impl Future<Output = Result<String>> + Send;

    /// The token to send again after `refused` got a 401: a new one, or `refused` itself when there is none better,
    /// and then the 401 is the answer.
    ///
    /// # Errors
    ///
    /// When no token can be had: the call fails with this error as the source of its own.
    fn refresh(&self, refused: &str) -> impl Future<Output = Result<String>> + Send;
}

/// Sends `Authorization: Bearer <token>`, with a token asked of `source` before each call.
///
/// It sends it on the requests whose [`Route`] takes [`Bearer`], to the api's own origin, as
/// [`bearer`](super::bearer) does.
///
/// A call answered 401 is sent once more with the token [`TokenSource::refresh`] gives, and its second answer is the
/// call's, a 401 again included. It is not sent again when the source gives back the token refused, or when its body
/// cannot be sent twice, a stream of the application's own, which no route of a contract sends: the 401 answers then.
/// A token that is not a `b64token`, letters, digits, `-._~+/` and `=` at its end, fails the call with an error that
/// does not show it.
///
/// ```no_run
/// # use quoll::client::{self, TokenSource};
/// # use quoll::rest;
/// # struct Session;
/// # impl TokenSource for Session {
/// #     async fn token(&self) -> rest::Result<String> { Ok(String::new()) }
/// #     async fn refresh(&self, _: &str) -> rest::Result<String> { Ok(String::new()) }
/// # }
/// let http = client::builder()
///     .with(client::bearer_from(Session))
///     .build_http()?;
/// # Ok::<(), quoll::rest::Error>(())
/// ```
pub fn bearer_from(source: impl TokenSource) -> impl Middleware {
    let source = Arc::new(source);

    helper(Rank::Credential, move |request, extensions, next| {
        if !own_route(&request, extensions).is_some_and(Route::takes::<Bearer>) {
            return next.run(request, extensions);
        }

        let source = Arc::clone(&source);

        Box::pin(async move {
            let token = source
                .token()
                .await
                .map_err(reqwest_middleware::Error::middleware)?;
            // A copy taken before the token is put in, sent again after a 401; `None` for a body that streams.
            let again = request.try_clone();

            let response = next
                .clone()
                .run(with_token(request, &token)?, extensions)
                .await?;

            if response.status() != StatusCode::UNAUTHORIZED {
                return Ok(response);
            }

            let Some(again) = again else {
                return Ok(response);
            };

            let fresh = source
                .refresh(&token)
                .await
                .map_err(reqwest_middleware::Error::middleware)?;

            if fresh == token {
                return Ok(response);
            }

            drop(response);
            next.run(with_token(again, &fresh)?, extensions).await
        })
    })
}

/// `request` with `Authorization: Bearer <token>`, or an error that does not show a token that is not a `b64token`.
fn with_token(mut request: Request, token: &str) -> reqwest_middleware::Result<Request> {
    let value = bearer_header(token).ok_or_else(|| {
        Failure::fail(
            "the bearer token of the source cannot be sent: it is not a `b64token`, letters, digits, `-._~+/` and `=` at its end",
        )
    })?;

    request.headers_mut().insert(AUTHORIZATION, value);
    Ok(request)
}
