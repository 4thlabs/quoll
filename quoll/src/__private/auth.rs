//! What the generated code calls for the schemes a contract declares: how each one reads its credential, and where it
//! travels, which the client's `rest::client::key` reads.

use http::request::Parts;
use http::{Method, StatusCode, header};

use crate::rest::{Bearer, Error, Result, Scheme};

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

/// A scheme the contract declares, which the macro implements: where its credential travels.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not a scheme the contract declares, whose credential `rest::client::key` sends",
    label = "not a declared scheme",
    note = "`rest::client::bearer` sends a `Bearer` token and `rest::client::basic` a `Basic` user and password; a scheme of the application's own sends its credential through a middleware of its own"
)]
pub trait Declared: Scheme {
    /// Where its credential travels.
    const PLACE: Place;
}

/// Checks, at compile time, that `S` is a scheme: the type a contract names in `#[auth(...)]`.
pub const fn is_scheme<S: Scheme>() {}

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

/// The credential of a cookie scheme: the value of the cookie `name`, as it is. An empty cookie, as one cleared by
/// the application, is none.
///
/// A request that carries the cookie is checked as coming from a page of the server's own origin, so that another
/// site cannot have a browser send it, the browser adding the cookie on its own: see [`cross_origin`].
///
/// # Errors
///
/// A 403 for a request with the cookie from another origin. A 401, whose detail never shows the value, when the
/// cookie is given twice, or is not UTF-8.
pub fn cookie(request: &Parts, name: &'static str) -> Result<Option<String>> {
    let mut found: Option<&[u8]> = None;

    for value in request.headers.get_all(header::COOKIE) {
        for pair in value.as_bytes().split(|&byte| byte == b';') {
            let pair = pair.trim_ascii();
            let Some(equals) = pair.iter().position(|&byte| byte == b'=') else {
                continue;
            };

            if pair[..equals].trim_ascii() != name.as_bytes() {
                continue;
            }

            if found.is_some() {
                return Err(Error::unauthorized(format!(
                    "the cookie `{name}` is given more than once"
                )));
            }

            found = Some(pair[equals + 1..].trim_ascii());
        }
    }

    let Some(value) = found.filter(|value| !value.is_empty()) else {
        return Ok(None);
    };

    cross_origin(request)?;

    let text = std::str::from_utf8(value)
        .map_err(|_| Error::unauthorized(format!("the cookie `{name}` is not UTF-8")))?;

    Ok(Some(text.to_owned()))
}

/// Refuses a request that a page of another origin may have had a browser send, with its cookies: a request whose
/// method is not `GET`, `HEAD` or `OPTIONS` is from the server's own origin, as its `Sec-Fetch-Site` says, or, from a
/// browser that does not send it, as its `Origin` says. A request with neither header comes from no browser, and
/// passes.
///
/// # Errors
///
/// A 403 for a request from another origin.
pub fn cross_origin(request: &Parts) -> Result<()> {
    if matches!(request.method, Method::GET | Method::HEAD | Method::OPTIONS) {
        return Ok(());
    }

    let refused = || {
        Error::new(
            StatusCode::FORBIDDEN,
            "the request comes from another origin, which a cookie does not authenticate",
        )
    };

    if let Some(site) = request.headers.get("sec-fetch-site") {
        return match site.as_bytes() {
            b"same-origin" | b"none" => Ok(()),
            _ => Err(refused()),
        };
    }

    let Some(origin) = request.headers.get(header::ORIGIN) else {
        return Ok(());
    };
    let host = request
        .headers
        .get(header::HOST)
        .map(|host| host.as_bytes())
        .or_else(|| {
            request
                .uri
                .authority()
                .map(|authority| authority.as_str().as_bytes())
        });
    let origin_host = origin
        .as_bytes()
        .split(|&byte| byte == b'/')
        .nth(2)
        .filter(|_| origin.as_bytes().starts_with(b"http"));

    match (origin_host, host) {
        (Some(origin_host), Some(host)) if origin_host.eq_ignore_ascii_case(host) => Ok(()),
        _ => Err(refused()),
    }
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

        let request = parts(http::Request::get("/").header("cookie", "session=a; session=b"));
        let error = cookie(&request, "session").expect_err("twice");
        assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
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
