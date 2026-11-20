//! `cors(origins)`: the pages of other origins a browser lets call the api.

use std::convert::Infallible;
use std::error::Error as StdError;
use std::fmt;
use std::net::{Ipv4Addr, Ipv6Addr};
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
///   read case aside, an IPv6 address is compressed, `[::1]`, and a default port is dropped, as a browser sends them.
///   A host a browser reads as an IPv4 address is written as one, `127.0.0.1`.
/// - Every method a contract serves is allowed: `GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE` and `OPTIONS`. Any
///   header a page asks for is allowed too, as the routes' headers, a scheme's and `X-Request-Id` are the
///   application's: the origins listed are trusted.
/// - A preflight is an `OPTIONS` request with an `Origin` and an `Access-Control-Request-Method`, which the layer
///   answers itself. Any other `OPTIONS` request is a call of the route, which answers it, with the headers of CORS.
/// - A page reads `Location`, `WWW-Authenticate` and `X-Request-Id` in an answer, beside the headers every page reads.
/// - Credentials are not allowed: a page of another origin sends a token or a key in a header, which needs none, and
///   no cookie. A cookie scheme trusts the server's own origin alone: a call of another origin with its cookie whose
///   method is not `GET`, `HEAD` or `OPTIONS` is answered 403, whether its origin is listed here or not.
/// - A browser keeps the answer of a preflight for an hour.
///
/// ```
/// use quoll::server;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// # fn main() -> Result<(), server::InvalidOrigin> {
/// let router = server::builder()
///     .with(server::cors(["https://app.example.com"])?)
///     .into_router(api_router());
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
            Method::OPTIONS,
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

    // `http` does not check the port: `:`, `:99999` or `:+80` would read as no port, the scheme's default.
    let port = match &authority.as_str()[authority.host().len()..] {
        "" => None,
        after_host => {
            let digits = after_host
                .strip_prefix(':')
                .filter(|digits| {
                    !digits.is_empty() && digits.bytes().all(|byte| byte.is_ascii_digit())
                })
                .ok_or_else(invalid)?;

            Some(digits.parse::<u16>().map_err(|_| invalid())?)
        }
    };
    let host = host_value(authority.host()).ok_or_else(invalid)?;
    let text = match port {
        Some(port) if port != default_port => format!("{scheme}://{host}:{port}"),
        _ => format!("{scheme}://{host}"),
    };

    HeaderValue::try_from(text).map_err(|_| invalid())
}

/// `host` as a browser writes it in `Origin`, which the URL standard serializes: a name in lowercase, an IPv6 address
/// compressed, `[::1]`. `None` for a host a browser would read as an IPv4 address written otherwise than as one,
/// `127.1` or `0x7f.0.0.1`, and for an IPv6 address that is not one.
fn host_value(host: &str) -> Option<String> {
    if let Some(address) = host
        .strip_prefix('[')
        .and_then(|host| host.strip_suffix(']'))
    {
        let address: Ipv6Addr = address.parse().ok()?;

        return Some(format!("[{}]", ipv6_text(address)));
    }

    let host = host.to_ascii_lowercase();
    // The URL standard reads a host whose last label is a number as an IPv4 address.
    let last = host
        .trim_end_matches('.')
        .rsplit('.')
        .next()
        .unwrap_or_default();
    let numeric = !last.is_empty()
        && (last.bytes().all(|byte| byte.is_ascii_digit()) || last.starts_with("0x"));

    if numeric && host.parse::<Ipv4Addr>().is_err() {
        return None;
    }

    Some(host)
}

/// An IPv6 address as the URL standard writes it: its eight pieces in lowercase hex, the first of its longest runs of
/// two zero pieces or more as `::`, and never an IPv4 address inside, as Rust's `Display` writes `::ffff:1.2.3.4`.
fn ipv6_text(address: Ipv6Addr) -> String {
    let pieces = address.segments();
    let mut longest = (0, 0);
    let mut index = 0;

    while index < pieces.len() {
        let start = index;

        while index < pieces.len() && pieces[index] == 0 {
            index += 1;
        }

        if index - start > longest.1 {
            longest = (start, index - start);
        }

        index += 1;
    }

    let hex = |pieces: &[u16]| {
        pieces
            .iter()
            .map(|piece| format!("{piece:x}"))
            .collect::<Vec<_>>()
            .join(":")
    };
    let (start, length) = longest;

    if length < 2 {
        return hex(&pieces);
    }

    format!(
        "{}::{}",
        hex(&pieces[..start]),
        hex(&pieces[start + length..])
    )
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
        CorsService(self.0.layer(RestoreOptions(inner)))
    }
}

/// The service of [`CorsLayer`].
#[derive(Debug, Clone)]
pub struct CorsService<S>(Cors<RestoreOptions<S>>);

/// Whether `request` is a preflight, as a browser sends one before a call: tower-http's `Cors` takes any `OPTIONS`
/// request for one, which would answer a route's own `OPTIONS` in its place.
fn is_preflight(request: &Request) -> bool {
    let headers = request.headers();

    request.method() == Method::OPTIONS
        && headers.contains_key(header::ORIGIN)
        && headers.contains_key(header::ACCESS_CONTROL_REQUEST_METHOD)
}

/// Marks an `OPTIONS` call of a route, given to tower-http's `Cors` as a `GET`, so that it gets the headers of a call
/// and reaches the route, where [`RestoreOptions`] gives it back its method.
#[derive(Debug, Clone, Copy)]
struct ActualOptions;

/// The route under [`CorsService`], which gives an `OPTIONS` call back its method.
#[derive(Debug, Clone)]
struct RestoreOptions<S>(S);

impl<S: Service<Request>> Service<Request> for RestoreOptions<S> {
    type Response = S::Response;
    type Error = S::Error;
    type Future = S::Future;

    fn poll_ready(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), S::Error>> {
        self.0.poll_ready(cx)
    }

    fn call(&mut self, mut request: Request) -> S::Future {
        if request.extensions_mut().remove::<ActualOptions>().is_some() {
            *request.method_mut() = Method::OPTIONS;
        }

        self.0.call(request)
    }
}

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

    fn call(&mut self, mut request: Request) -> ResponseFuture {
        if request.method() == Method::OPTIONS && !is_preflight(&request) {
            *request.method_mut() = Method::GET;
            request.extensions_mut().insert(ActualOptions);
        }

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
        assert_eq!(
            written("http://127.0.0.1:5173").as_deref(),
            Ok("http://127.0.0.1:5173")
        );
    }

    #[test]
    fn an_ipv6_address_is_written_compressed_as_a_browser_sends_it() {
        for (given, sent) in [
            ("http://[0:0:0:0:0:0:0:1]:5173", "http://[::1]:5173"),
            ("http://[::1]", "http://[::1]"),
            (
                "http://[2001:0DB8:0:0:1:0:0:1]",
                "http://[2001:db8::1:0:0:1]",
            ),
            ("http://[2001:db8:0:0:0:0:0:0]", "http://[2001:db8::]"),
            ("http://[0:0:0:0:0:0:0:0]", "http://[::]"),
            ("http://[1:0:1:0:1:0:1:0]", "http://[1:0:1:0:1:0:1:0]"),
            ("http://[::ffff:1.2.3.4]", "http://[::ffff:102:304]"),
        ] {
            assert_eq!(written(given).as_deref(), Ok(sent), "{given}");
        }
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
            "https://app.example.com:",
            "https://app.example.com:99999",
            "https://app.example.com:+80",
            "http://127.1",
            "http://0x7f.0.0.1",
            "http://127.000.0.1",
            "http://[::1::2]",
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
