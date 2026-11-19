//! `cors(origins)`: the pages of other origins a browser lets call the api.

use std::convert::Infallible;
use std::error::Error as StdError;
use std::fmt;
use std::task::{Context, Poll};
use std::time::Duration;

use axum::extract::Request;
use axum::response::Response;
use http::{HeaderName, HeaderValue, Method, Uri, header};
use tower_http::cors::{AllowHeaders, AllowOrigin, Cors};
use tower_layer::Layer;
use tower_service::Service;

use super::layer::ResponseFuture;

/// How long a browser keeps the answer of a preflight, rather than asking again before each call.
const MAX_AGE: Duration = Duration::from_secs(60 * 60);

/// CORS for the pages of `origins`, on tower-http's `Cors`: a browser lets a page of one of them call the api and
/// read its answers, and a page of any other origin neither.
///
/// - An origin is written `https://app.example.com` or `http://localhost:5173`: a scheme, `http` or `https`, a host
///   and a port when it is not the scheme's default, with no path, no user and no query. The scheme and the host are
///   read case aside, and a default port is dropped, as a browser sends them.
/// - Every method a contract serves is allowed: `GET`, `HEAD`, `POST`, `PUT`, `PATCH` and `DELETE`. Any header a page
///   asks for is allowed too, as the routes' headers, a scheme's and `X-Request-Id` are the application's: the origins
///   listed are trusted.
/// - A page reads `Location`, `WWW-Authenticate` and `X-Request-Id` in an answer, beside the headers every page reads.
/// - Credentials are not allowed: a page of another origin sends a token or a key in a header, which needs none, and
///   no cookie. A cookie scheme trusts the server's own origin alone: a call of another origin with its cookie whose
///   method is not `GET`, `HEAD` or `OPTIONS` is answered 403, whether its origin is listed here or not.
/// - A browser keeps the answer of a preflight for an hour.
///
/// ```
/// use quoll::rest;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// # fn main() -> Result<(), rest::server::InvalidOrigin> {
/// let router = api_router().layer(rest::server::cors(["https://app.example.com"])?);
/// # Ok(())
/// # }
/// ```
///
/// # Errors
///
/// [`InvalidOrigin`] for the first of `origins` that is not an origin, as `*`, `null` or `https://example.com/app`.
pub fn cors(
    origins: impl IntoIterator<Item = impl AsRef<str>>,
) -> Result<CorsLayer, InvalidOrigin> {
    let origins = origins
        .into_iter()
        .map(|origin| origin_value(origin.as_ref()))
        .collect::<Result<Vec<_>, _>>()?;

    let layer = tower_http::cors::CorsLayer::new()
        .allow_origin(AllowOrigin::list(origins))
        .allow_methods([
            Method::GET,
            Method::HEAD,
            Method::POST,
            Method::PUT,
            Method::PATCH,
            Method::DELETE,
        ])
        .allow_headers(AllowHeaders::mirror_request())
        .expose_headers([
            header::LOCATION,
            header::WWW_AUTHENTICATE,
            HeaderName::from_static("x-request-id"),
        ])
        .max_age(MAX_AGE);

    Ok(CorsLayer(layer))
}

/// `origin` as a browser writes it in `Origin`: its scheme and its host in lowercase, and its port unless it is the
/// scheme's default.
fn origin_value(origin: &str) -> Result<HeaderValue, InvalidOrigin> {
    let invalid = || InvalidOrigin {
        origin: origin.to_owned(),
    };
    let uri: Uri = origin.parse().map_err(|_| invalid())?;

    let (Some(scheme), Some(authority)) = (uri.scheme_str(), uri.authority()) else {
        return Err(invalid());
    };

    // `http` reads a URI with no path as one whose path is `/`: the text tells them apart.
    if origin.len() != scheme.len() + "://".len() + authority.as_str().len() {
        return Err(invalid());
    }
    let scheme = scheme.to_ascii_lowercase();
    let default_port = match scheme.as_str() {
        "http" => 80,
        "https" => 443,
        _ => return Err(invalid()),
    };

    if authority.as_str().contains('@') || authority.host().is_empty() {
        return Err(invalid());
    }

    let host = authority.host().to_ascii_lowercase();
    let text = match authority.port_u16() {
        Some(port) if port != default_port => format!("{scheme}://{host}:{port}"),
        _ => format!("{scheme}://{host}"),
    };

    HeaderValue::try_from(text).map_err(|_| invalid())
}

/// An origin given to [`cors`] that is not one.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InvalidOrigin {
    origin: String,
}

impl InvalidOrigin {
    /// The origin, as it was given.
    pub fn origin(&self) -> &str {
        &self.origin
    }
}

impl fmt::Display for InvalidOrigin {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "`{}` is not an origin: write `https://host`, or `https://host:port`, with no path",
            self.origin
        )
    }
}

impl StdError for InvalidOrigin {}

/// The layer [`cors`] gives.
#[derive(Debug, Clone)]
pub struct CorsLayer(tower_http::cors::CorsLayer);

impl<S> Layer<S> for CorsLayer {
    type Service = CorsService<S>;

    fn layer(&self, inner: S) -> Self::Service {
        CorsService(self.0.layer(inner))
    }
}

/// The service of [`CorsLayer`].
#[derive(Debug, Clone)]
pub struct CorsService<S>(Cors<S>);

impl<S> Service<Request> for CorsService<S>
where
    S: Service<Request, Response = Response, Error = Infallible> + Send + 'static,
    S::Future: Send + 'static,
{
    type Response = Response;
    type Error = Infallible;
    type Future = ResponseFuture;

    fn poll_ready(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), Infallible>> {
        self.0.poll_ready(cx)
    }

    fn call(&mut self, request: Request) -> ResponseFuture {
        Box::pin(self.0.call(request))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn written(origin: &str) -> Result<String, InvalidOrigin> {
        origin_value(origin).map(|value| value.to_str().expect("ASCII").to_owned())
    }

    #[test]
    fn an_origin_is_written_as_a_browser_sends_it() {
        assert_eq!(
            written("https://app.example.com").as_deref(),
            Ok("https://app.example.com")
        );
        assert_eq!(
            written("HTTPS://App.Example.COM:443").as_deref(),
            Ok("https://app.example.com")
        );
        assert_eq!(
            written("http://localhost:80").as_deref(),
            Ok("http://localhost")
        );
        assert_eq!(
            written("http://localhost:5173").as_deref(),
            Ok("http://localhost:5173")
        );
        assert_eq!(
            written("https://[::1]:8443").as_deref(),
            Ok("https://[::1]:8443")
        );
    }

    #[test]
    fn what_is_not_an_origin_is_refused() {
        for origin in [
            "",
            "*",
            "null",
            "app.example.com",
            "ftp://example.com",
            "https://example.com/",
            "https://example.com/app",
            "https://example.com?x=1",
            "https://ana@example.com",
            "https://",
        ] {
            let error = written(origin).expect_err(origin);

            assert_eq!(error.origin(), origin);
        }
    }

    #[test]
    fn the_error_says_what_to_write() {
        let error = cors(["https://example.com/app"]).expect_err("a path");

        assert_eq!(
            error.to_string(),
            "`https://example.com/app` is not an origin: write `https://host`, or `https://host:port`, with no path"
        );
    }
}
