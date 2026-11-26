//! What the generated code calls for the schemes a contract declares: how each one reads its credential, where it
//! travels, and how a client writes it.

use http::request::Parts;
use http::uri::Authority;
use http::{HeaderName, Method, StatusCode, Uri, header};

use crate::auth::{Outgoing, Unsendable};
use crate::{Bearer, ClientScheme, Error, Result, Scheme, ServerScheme};

/// Where the credential of a scheme the contract declares travels.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Place {
    /// In a header, whose name is lowercase: `scheme Key = header "X-Api-Key";`.
    Header(&'static str),
    /// In a cookie: `scheme Session = cookie "session";`.
    Cookie(&'static str),
    /// In a key of the query: `scheme Token = query "token";`.
    Query(&'static str),
    /// In `Authorization: Bearer`, as an OpenID Connect token: `scheme Login = openid "...";`.
    Bearer,
}

/// A scheme the contract declares, which the macro implements: what the api's checks know of it.
pub trait Declared: Scheme {
    /// Where its credential travels.
    const PLACE: Place;

    /// Whether it refuses a request from another origin that changes things: a cookie scheme.
    const CHECKS_ORIGIN: bool;

    /// How far its visibility reaches, as a group's: 0 private, 1 a module's, 2 the crate's, 3 everyone's.
    const REACH: u8;
}

/// Writes the key of a header scheme in the header `name`, lowercase.
///
/// # Errors
///
/// When the key is empty, or holds a control character or a space at either end.
pub fn write_header(
    request: &mut Outgoing<'_>,
    name: &'static str,
    key: &str,
) -> std::result::Result<(), Unsendable> {
    request
        .header(HeaderName::from_static(name), key)
        .map_err(|_| {
            Unsendable::new(
                "the key cannot be sent in its header: it is empty, or holds a control character or a space at either end",
            )
        })
}

/// Writes the key of a cookie scheme in the cookie `name`.
///
/// # Errors
///
/// When the key is empty, or holds a character a cookie's value cannot.
pub fn write_cookie(
    request: &mut Outgoing<'_>,
    name: &'static str,
    key: &str,
) -> std::result::Result<(), Unsendable> {
    request.cookie(name, key).map_err(|_| {
        Unsendable::new(
            "the key cannot be sent as a cookie: it is empty, or holds a character a cookie's value cannot",
        )
    })
}

/// Writes the key of a query scheme in the key `name` of the query.
///
/// # Errors
///
/// When the key is empty.
pub fn write_query(
    request: &mut Outgoing<'_>,
    name: &'static str,
    key: &str,
) -> std::result::Result<(), Unsendable> {
    request
        .query_pair(name, key)
        .map_err(|_| Unsendable::new("the key cannot be sent in the query: it is empty"))
}

/// Writes the token of an OpenID Connect scheme in `Authorization: Bearer`.
///
/// # Errors
///
/// When the token is not a `b64token`.
pub fn write_openid(
    request: &mut Outgoing<'_>,
    token: &str,
) -> std::result::Result<(), Unsendable> {
    request.authorization_token68("Bearer", token).map_err(|_| {
        Unsendable::new(
            "the OpenID Connect token cannot be sent: it is not a `b64token`, letters, digits, `-._~+/` and `=` at its end",
        )
    })
}

/// Checks, at compile time, that `S` is a scheme a client writes: a type an api that calls names in `#[auth(...)]`.
pub const fn is_client_scheme<S: ClientScheme>() {}

/// The credential of a header scheme: the value of the header `name`, lowercase.
///
/// # Errors
///
/// A 401, whose detail never shows the value, when the header is given twice, is empty or is not UTF-8.
pub fn header(request: &Parts, name: &'static str) -> Result<Option<String>> {
    let mut values = request.headers.get_all(name).iter();
    let Some(value) = values.next() else {
        return Ok(None);
    };

    if values.next().is_some() {
        return Err(Error::unauthorized(format!(
            "the header `{name}` is given more than once"
        )));
    }

    let text = std::str::from_utf8(value.as_bytes())
        .map_err(|_| Error::unauthorized(format!("the header `{name}` is not UTF-8")))?
        .trim();

    if text.is_empty() {
        return Err(Error::unauthorized(format!("the header `{name}` is empty")));
    }

    Ok(Some(text.to_owned()))
}

/// The credential of a cookie scheme: the first value of the cookie `name` that is not empty, as it is. A browser that
/// holds the cookie for two paths or two domains sends both, the one of the longest path first (RFC 6265, 5.4); an
/// empty cookie, as one cleared by the application, is none.
///
/// A request that carries the cookie is checked as coming from a page of the server's own origin, so that another
/// site cannot have a browser send it, the browser adding the cookie on its own: see [`cross_origin`].
///
/// # Errors
///
/// A 403 for a request with the cookie from another origin. A 401, whose detail never shows the value, when the
/// cookie is not UTF-8.
pub fn cookie(request: &Parts, name: &'static str) -> Result<Option<String>> {
    let found = request
        .headers
        .get_all(header::COOKIE)
        .iter()
        .flat_map(|value| value.as_bytes().split(|&byte| byte == b';'))
        .find_map(|pair| {
            let pair = pair.trim_ascii();
            let equals = pair.iter().position(|&byte| byte == b'=')?;
            let value = pair[equals + 1..].trim_ascii();

            (pair[..equals].trim_ascii() == name.as_bytes() && !value.is_empty()).then_some(value)
        });

    let Some(value) = found else {
        return Ok(None);
    };

    cross_origin(request)?;

    let text = std::str::from_utf8(value)
        .map_err(|_| Error::unauthorized(format!("the cookie `{name}` is not UTF-8")))?;

    Ok(Some(text.to_owned()))
}

/// Refuses a request that a page of another origin may have had a browser send, with its cookies: a request whose
/// method is not `GET`, `HEAD` or `OPTIONS` is from the server's own origin, as its `Sec-Fetch-Site` says, or, from a
/// browser that does not send it, as its `Origin` says, compared with its `Host`: one host, and one port, the default
/// of the origin's scheme when either leaves it out. A request with neither header comes from no browser, and passes.
///
/// Behind a reverse proxy, the proxy passes the `Host` the browser sent, as nginx does with `proxy_set_header Host
/// $host`: `X-Forwarded-Host`, which anyone may send, is not read.
///
/// # Errors
///
/// A 403 for a request from another origin.
pub fn cross_origin(request: &Parts) -> Result<()> {
    if matches!(request.method, Method::GET | Method::HEAD | Method::OPTIONS) {
        return Ok(());
    }

    if let Some(site) = request.headers.get("sec-fetch-site") {
        return match site.as_bytes() {
            b"same-origin" | b"none" => Ok(()),
            _ => Err(refused()),
        };
    }

    let Some(origin) = request.headers.get(header::ORIGIN) else {
        return Ok(());
    };
    let host = match request.headers.get(header::HOST) {
        Some(host) => host
            .to_str()
            .ok()
            .and_then(|host| host.parse::<Authority>().ok()),
        None => request.uri.authority().cloned(),
    };
    let origin = origin
        .to_str()
        .ok()
        .and_then(|origin| origin.parse::<Uri>().ok());

    match (origin, host) {
        (Some(origin), Some(host)) if same_origin(&origin, &host) => Ok(()),
        _ => Err(refused()),
    }
}

/// The answer to a request from another origin.
fn refused() -> Error {
    Error::new(
        StatusCode::FORBIDDEN,
        "the request comes from another origin, which a cookie does not authenticate",
    )
}

/// Whether `origin`, an `Origin` header's, is the one of a request to `host`: the same host, whatever its case, and
/// the same port, the default of the origin's scheme where either does not write it.
fn same_origin(origin: &Uri, host: &Authority) -> bool {
    let default = match origin.scheme_str() {
        Some("https") => 443,
        Some("http") => 80,
        _ => return false,
    };
    let Some(origin_host) = origin.host() else {
        return false;
    };
    let is_bare = origin
        .path_and_query()
        .is_none_or(|path| path.as_str() == "/");

    is_bare
        && origin_host.eq_ignore_ascii_case(host.host())
        && origin.port_u16().unwrap_or(default) == host.port_u16().unwrap_or(default)
}

/// The credential of a query scheme: the value of the key `key`, decoded as a form's.
///
/// # Errors
///
/// A 401, whose detail never shows the value, when the key is given twice, is empty or is not UTF-8 once decoded.
pub fn query(request: &Parts, key: &'static str) -> Result<Option<String>> {
    let query = request.uri.query().unwrap_or_default();
    let mut found: Option<String> = None;

    for pair in query.split('&') {
        let (name, value) = pair.split_once('=').unwrap_or((pair, ""));

        if decode(name).as_deref() != Some(key) {
            continue;
        }

        if found.is_some() {
            return Err(Error::unauthorized(format!(
                "the query key `{key}` is given more than once"
            )));
        }

        let value = decode(value)
            .ok_or_else(|| Error::unauthorized(format!("the query key `{key}` is not UTF-8")))?;
        found = Some(value);
    }

    match found {
        Some(value) if value.is_empty() => Err(Error::unauthorized(format!(
            "the query key `{key}` is empty"
        ))),
        found => Ok(found),
    }
}

/// A key or a value of a query, decoded as a form's: `+` is a space, then each `%XX` its byte. `None` when it is not
/// UTF-8 once decoded.
fn decode(text: &str) -> Option<String> {
    let spaced = text.replace('+', " ");

    percent_encoding::percent_decode_str(&spaced)
        .decode_utf8()
        .ok()
        .map(|decoded| decoded.into_owned())
}

/// The credential of an OpenID Connect scheme: a token in `Authorization: Bearer`, read as [`Bearer`] reads it.
///
/// # Errors
///
/// As [`Bearer`]'s.
pub fn openid(request: &Parts) -> Result<Option<String>> {
    Bearer::read(request)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parts(request: http::request::Builder) -> Parts {
        request.body(()).expect("a valid request").into_parts().0
    }

    #[test]
    fn a_header_scheme_reads_its_header_once() {
        let request = parts(http::Request::get("/").header("x-api-key", " k-1 "));
        assert_eq!(
            header(&request, "x-api-key").expect("a key").as_deref(),
            Some("k-1")
        );

        let request = parts(http::Request::get("/"));
        assert_eq!(header(&request, "x-api-key").expect("no key"), None);

        for request in [
            http::Request::get("/")
                .header("x-api-key", "a")
                .header("X-Api-Key", "b"),
            http::Request::get("/").header("x-api-key", ""),
            http::Request::get("/").header(
                "x-api-key",
                http::HeaderValue::from_bytes(b"caf\xe9").expect("a value"),
            ),
        ] {
            let error = header(&parts(request), "x-api-key").expect_err("refused");

            assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
        }
    }

    #[test]
    fn a_cookie_scheme_reads_its_cookie_among_the_others() {
        let request = parts(
            http::Request::get("/")
                .header("cookie", "theme=dark; session=s-1")
                .header("cookie", "other=1"),
        );
        assert_eq!(
            cookie(&request, "session").expect("a cookie").as_deref(),
            Some("s-1")
        );

        for value in ["theme=dark", "sessions=x", "session=", ""] {
            let request = parts(http::Request::get("/").header("cookie", value));

            assert_eq!(cookie(&request, "session").expect("none"), None, "{value}");
        }

        // A browser sends the cookie of the longest path first: it is the one read.
        let request = parts(
            http::Request::get("/")
                .header("cookie", "session=; session=a")
                .header("cookie", "session=b"),
        );
        assert_eq!(
            cookie(&request, "session").expect("the first").as_deref(),
            Some("a")
        );
    }

    /// What `cross_origin` makes of a request with these headers.
    fn from(method: &str, headers: &[(&str, &str)]) -> Option<StatusCode> {
        let mut request = http::Request::builder()
            .method(method)
            .uri("/api/notes")
            .header("host", "notes.example.com");
        for (name, value) in headers {
            request = request.header(*name, *value);
        }

        cross_origin(&parts(request))
            .err()
            .and_then(|error| error.status())
    }

    #[test]
    fn a_request_from_another_origin_is_refused_unless_its_method_is_safe() {
        let forbidden = Some(StatusCode::FORBIDDEN);

        assert_eq!(from("POST", &[("sec-fetch-site", "same-origin")]), None);
        assert_eq!(from("POST", &[("sec-fetch-site", "none")]), None);
        assert_eq!(from("POST", &[("sec-fetch-site", "cross-site")]), forbidden);
        assert_eq!(
            from("DELETE", &[("sec-fetch-site", "same-site")]),
            forbidden
        );
        assert_eq!(from("GET", &[("sec-fetch-site", "cross-site")]), None);
        assert_eq!(from("HEAD", &[("origin", "https://evil.example")]), None);
        assert_eq!(from("OPTIONS", &[("origin", "https://evil.example")]), None);

        assert_eq!(from("POST", &[]), None, "no browser");
        assert_eq!(
            from("PUT", &[("origin", "https://notes.example.com")]),
            None
        );
        assert_eq!(
            from("PUT", &[("origin", "https://NOTES.example.com")]),
            None
        );
        assert_eq!(
            from("PUT", &[("origin", "https://evil.example")]),
            forbidden
        );
        assert_eq!(
            from("PUT", &[("origin", "https://notes.example.com.evil")]),
            forbidden
        );
        assert_eq!(from("PATCH", &[("origin", "null")]), forbidden);
        assert_eq!(
            from("PUT", &[("origin", "https://notes.example.com:443")]),
            None,
            "the default port, written"
        );
        assert_eq!(
            from("PUT", &[("origin", "http://notes.example.com:80")]),
            None
        );
        assert_eq!(
            from("PUT", &[("origin", "https://notes.example.com:8443")]),
            forbidden,
            "another port"
        );
        assert_eq!(from("PUT", &[("origin", "notes.example.com")]), forbidden);
        assert_eq!(
            from("PUT", &[("origin", "https://notes.example.com/path")]),
            forbidden
        );
        assert_eq!(
            from(
                "POST",
                &[
                    ("sec-fetch-site", "cross-site"),
                    ("origin", "https://notes.example.com")
                ]
            ),
            forbidden,
            "the fetch metadata comes first"
        );
    }

    /// What `cross_origin` makes of a request to `host` from the page of `origin`.
    fn to(host: &str, origin: &str) -> Option<StatusCode> {
        let request = http::Request::post("/api/notes")
            .header("host", host)
            .header("origin", origin);

        cross_origin(&parts(request))
            .err()
            .and_then(|error| error.status())
    }

    #[test]
    fn an_origin_is_compared_with_the_host_as_an_origin() {
        let forbidden = Some(StatusCode::FORBIDDEN);

        assert_eq!(
            to("notes.example.com:443", "https://notes.example.com"),
            None
        );
        assert_eq!(
            to("notes.example.com:8080", "http://notes.example.com:8080"),
            None
        );
        assert_eq!(to("localhost:5173", "http://localhost:5173"), None);
        assert_eq!(to("[::1]:8080", "http://[::1]:8080"), None);
        assert_eq!(
            to("notes.example.com:80", "https://notes.example.com"),
            forbidden
        );
        // A proxy that sends its upstream's address as the `Host` is set to pass the browser's.
        assert_eq!(to("127.0.0.1:8080", "https://notes.example.com"), forbidden);
    }

    #[test]
    fn a_cookie_from_another_origin_is_a_403_and_none_is_no_check() {
        let cross = http::Request::post("/")
            .header("host", "notes.example.com")
            .header("sec-fetch-site", "cross-site");

        let error = cookie(&parts(cross.header("cookie", "session=s-1")), "session")
            .expect_err("cross-site");
        assert_eq!(error.status(), Some(StatusCode::FORBIDDEN));

        let without = http::Request::post("/")
            .header("host", "notes.example.com")
            .header("sec-fetch-site", "cross-site");
        assert_eq!(cookie(&parts(without), "session").expect("no cookie"), None);
    }

    #[test]
    fn a_query_scheme_reads_its_key_decoded() {
        let request = parts(http::Request::get("/?page=2&token=a%2Bb+c&x"));
        assert_eq!(
            query(&request, "token").expect("a token").as_deref(),
            Some("a+b c")
        );

        let request = parts(http::Request::get("/?page=2"));
        assert_eq!(query(&request, "token").expect("none"), None);

        for uri in ["/?token=a&token=b", "/?token=", "/?token", "/?token=%FF"] {
            let error = query(&parts(http::Request::get(uri)), "token").expect_err(uri);

            assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED), "{uri}");
        }
    }
}
