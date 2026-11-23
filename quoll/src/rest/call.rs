//! A call of a route, described and not sent yet: what every method of a contract's client returns.

use std::future::{Future, IntoFuture};
use std::time::Duration;

use crate::rest::{Problem, Response, Result};

/// A call of a route that returns `T`, described and not sent yet: what every method of a contract's client
/// returns. `.await` sends it and gives the route's value, or an error of the shape `E`, the api's, an RFC 9457
/// [`Problem`] unless the contract says `#[error(E)]`; its methods change it first.
///
/// ```no_run
/// # use std::time::Duration;
/// use http::StatusCode;
/// use quoll::rest::{self, Call};
///
/// rest::api! {
///     pub api Api = "/api";
///
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> String;
///     }
/// }
///
/// # async fn run() -> rest::Result<()> {
/// let api = Api::client("http://localhost:8080")?;
///
/// // The value, or an error that goes up.
/// let title = api.notes().get(3).await?;
///
/// // The whole answer: its status, its headers, and its body read as the value or the error.
/// let response = api.notes().get(4).timeout(Duration::from_secs(2)).response().await?;
/// let remaining = response.headers().get("x-ratelimit-remaining").cloned();
///
/// if response.status() == StatusCode::NOT_FOUND {
///     return Ok(());
/// }
///
/// let title = response.into_value()?;
/// # let _ = (title, remaining);
/// # Ok(())
/// # }
/// ```
///
/// An api that answers its errors in a shape of its own says it with `#[error(E)]`: its calls give a
/// `rest::Result<T, E>`, whose error has the `E` the answer's body is, its [`detail`](crate::rest::Error::detail).
///
/// ```no_run
/// use std::fmt;
///
/// use quoll::rest;
/// use serde::Deserialize;
///
/// /// What GitHub answers on an error.
/// #[derive(Debug, Deserialize)]
/// pub struct GitHubError {
///     pub message: String,
///     pub documentation_url: Option<String>,
/// }
///
/// impl fmt::Display for GitHubError {
///     fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
///         f.write_str(&self.message)
///     }
/// }
///
/// rest::api! {
///     #[openapi(none)]
///     #[error(GitHubError)]
///     pub api GitHub = "";
///
///     pub group Repos {
///         GET "/repos/{owner}/{repo}/topics" => topics(owner: String, repo: String) -> Vec<String>;
///     }
/// }
///
/// # async fn run() -> rest::Result<()> {
/// let github = GitHub::client("https://api.github.com")?;
///
/// match github.repos().topics("rust-lang".into(), "rust".into()).await {
///     Ok(topics) => println!("{topics:?}"),
///     Err(error) => match error.detail() {
///         Some(GitHubError { documentation_url: Some(url), .. }) => println!("{error}, see {url}"),
///         _ => return Err(error.into()),
///     },
/// }
/// # Ok(())
/// # }
/// ```
///
/// Any future of a `rest::Result<T, E>` is a `Call<T, E>` too, so that a fake of a group's trait returns an
/// `async move` block: its `.timeout` does nothing, and its `.response()` gives a 200 with no headers, or the error
/// with the status it has, as an answer of that status, and an error with none, a call that got no answer, as it is. A
/// 200 even for a route that the server answers 201 or 202: a fake has no contract's status to read. A fake of an api
/// with `#[error(E)]` builds the error of an answer with [`rest::Error::answered`](crate::rest::Error::answered), and
/// one of a status alone with [`into_shape`](crate::rest::Error::into_shape). The `E` of such a future is the one its
/// `Err` has: a block that only returns `Ok`, where no signature says the shape, names it,
/// `async { Ok::<_, rest::Error>(3) }.timeout(duration)`.
///
/// ```
/// use quoll::rest::{self, Call};
///
/// rest::api! {
///     pub api Api = "/api";
///
///     pub group Notes {
///         GET "/notes/{id}" => get(id: i64) -> String;
///     }
/// }
///
/// struct FakeNotes;
///
/// impl Notes for FakeNotes {
///     fn get(&self, id: i64) -> impl Call<String> {
///         async move {
///             if id == 1 {
///                 return Ok("Milk".to_owned());
///             }
///
///             Err(rest::Error::not_found("no such note"))
///         }
///     }
/// }
/// ```
#[must_use = "a call does nothing until it is awaited"]
pub trait Call<T, E = Problem>:
    IntoFuture<Output = Result<T, E>, IntoFuture: Send> + Send + Sized
{
    /// Sends the call, and gives the answer it got, whatever its status: its status, its headers, and its body read
    /// as the route's value on a success, or as the error of the answer otherwise, with its status, its detail when
    /// the body is an `E`, and its body. A 304 Not Modified is such an error, with no detail: it comes to a call that
    /// sent `If-None-Match`, whose answer's headers are these. A 204 or a 205 is a success, read as `()` or `None`: on
    /// a route whose value is not an `Option`, as another API may answer it, its body is the error of a body that is
    /// not the route's result, with no status, and the response keeps the answer's status and headers.
    /// [`Response::into_value`] gives the value, or that error, to go up with `?`.
    ///
    /// # Errors
    ///
    /// When the call got no answer it can read: a connection refused, a timeout, a middleware's error, or an answer
    /// with a success status but a 204 or a 205 whose body is not the route's result.
    fn response(self) -> impl Future<Output = Result<Response<Result<T, E>>, E>> + Send;

    /// A deadline of `duration` for this call alone, in place of the client's [`client::timeout`]: for each attempt
    /// and each hop, and for the head of the answer alone on a stream, as `client::timeout`'s, which
    /// [`client::retry`] tries again. A client with no `client::timeout` takes it too.
    ///
    /// [`client::timeout`]: crate::client::timeout
    /// [`client::retry`]: crate::client::retry
    #[must_use]
    fn timeout(self, duration: Duration) -> Self;
}

impl<T, E, F> Call<T, E> for F
where
    F: Future<Output = Result<T, E>> + Send,
{
    async fn response(self) -> Result<Response<Result<T, E>>, E> {
        let error = match self.await {
            Ok(value) => return Ok(Response::new(Ok(value))),
            Err(error) => error,
        };
        let Some(status) = error.status() else {
            return Err(error);
        };

        Ok(Response::from_parts(
            status,
            http::Version::HTTP_11,
            http::HeaderMap::new(),
            Err(error),
        ))
    }

    fn timeout(self, _duration: Duration) -> Self {
        self
    }
}

#[cfg(test)]
mod tests {
    use http::StatusCode;

    use super::*;
    use crate::rest::Error;

    #[tokio::test]
    async fn a_future_answers_a_value_as_a_200_with_no_headers() {
        let response = async { Ok::<_, Error>(3) }
            .timeout(Duration::from_millis(1))
            .response()
            .await
            .expect("an answer");

        assert_eq!(response.status(), StatusCode::OK);
        assert!(response.headers().is_empty());
        assert_eq!(response.into_value().expect("its value"), 3);
    }

    #[tokio::test]
    async fn a_future_answers_an_error_with_its_status() {
        let response = async { Err::<i64, _>(Error::not_found("no such note")) }
            .response()
            .await
            .expect("an answer");

        assert_eq!(response.status(), StatusCode::NOT_FOUND);
        assert_eq!(
            response.into_value().expect_err("its error").status(),
            Some(StatusCode::NOT_FOUND)
        );
    }

    #[tokio::test]
    async fn a_future_gives_an_error_with_no_status_as_no_answer() {
        let failed = Error::misuse("got no response", None);
        let error = async { Err::<i64, _>(failed) }
            .response()
            .await
            .expect_err("no answer");

        assert_eq!(error.status(), None);
    }
}
