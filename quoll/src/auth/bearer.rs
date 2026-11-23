//! The scheme `Bearer`: a token in the `Authorization` header, as RFC 6750 sends it.

use http::header::AUTHORIZATION;
use http::request::Parts;

use crate::openapi::SecurityScheme;
use crate::{Error, Result, Scheme};

/// A token in the `Authorization` header: `Authorization: Bearer <token>`, as RFC 6750 sends it. Its credential is the
/// token, a `String`.
///
/// The scheme's name is read whatever its case, `bearer` as `Bearer`, and the token is an RFC 6750 `b64token`:
/// letters, digits, `-._~+/`, and `=` at its end.
///
/// ```
/// use quoll::{Bearer, Scheme};
///
/// let request = http::Request::get("/api/notes")
///     .header("Authorization", "Bearer mF_9.B5f-4.1JqM")
///     .body(())
///     .unwrap();
/// let (parts, ()) = request.into_parts();
///
/// assert_eq!(Bearer::read(&parts).unwrap().as_deref(), Some("mF_9.B5f-4.1JqM"));
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Bearer;

impl Scheme for Bearer {
    type Credential = String;

    const NAME: &'static str = "Bearer";

    /// The token of the request's `Authorization: Bearer` header. `None` when the request has no `Authorization`
    /// header of the scheme `Bearer`: none at all, or only headers of another scheme, which this one leaves to the
    /// scheme they are for.
    ///
    /// # Errors
    ///
    /// A 401, whose detail never shows the header, when the request has an `Authorization: Bearer` header and:
    ///
    /// - another `Authorization` header beside it, which leaves it unclear what the request authenticates with;
    /// - an empty token;
    /// - a token that is not a `b64token`, as one with a space or a byte that is not ASCII.
    fn read(request: &Parts) -> Result<Option<String>> {
        let Some(token) = authorization(request, "Bearer")? else {
            return Ok(None);
        };

        if token.is_empty() {
            return Err(Error::unauthorized("the bearer token is empty"));
        }

        if !is_b64token(token) {
            return Err(Error::unauthorized(
                "the bearer token is not well formed: it is letters, digits, `-._~+/`, and `=` at its end",
            ));
        }

        // A `b64token` is ASCII, which the conversion keeps as it is.
        Ok(Some(String::from_utf8_lossy(token).into_owned()))
    }

    /// `{type: http, scheme: bearer}`.
    fn openapi() -> SecurityScheme {
        SecurityScheme::bearer()
    }
}

/// What follows the scheme `scheme` in the request's `Authorization` header, trimmed; `None` when the request has no
/// `Authorization` header of that scheme, whatever the case of its name: none at all, or only headers of another
/// scheme, which are left to the scheme they are for. Read as bytes, so that a header that is not ASCII is still known
/// by its scheme.
///
/// # Errors
///
/// A 401 when a header of the scheme has another `Authorization` header beside it, which leaves it unclear what the
/// request authenticates with.
pub(crate) fn authorization<'a>(request: &'a Parts, scheme: &str) -> Result<Option<&'a [u8]>> {
    let headers = request.headers.get_all(AUTHORIZATION);
    let Some(credentials) = headers
        .iter()
        .find_map(|value| credentials(value.as_bytes(), scheme))
    else {
        return Ok(None);
    };

    if headers.iter().nth(1).is_some() {
        return Err(Error::unauthorized(format!(
            "the request has an `Authorization: {scheme}` header and another `Authorization` header"
        )));
    }

    Ok(Some(credentials.trim_ascii()))
}

/// What follows the scheme in an `Authorization` header of the scheme `scheme`, whatever the case of its name; `None`
/// for a header of another scheme.
fn credentials<'a>(value: &'a [u8], scheme: &str) -> Option<&'a [u8]> {
    let (name, rest) = match value.iter().position(|&byte| byte == b' ') {
        Some(space) => (&value[..space], &value[space + 1..]),
        None => (value, &[][..]),
    };

    name.eq_ignore_ascii_case(scheme.as_bytes()).then_some(rest)
}

/// Whether `token` is an RFC 6750 `b64token`: `1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" ) *"="`. The
/// server reads no other token, and the client sends no other.
pub(crate) fn is_b64token(token: &[u8]) -> bool {
    let end = token
        .iter()
        .rposition(|&byte| byte != b'=')
        .map_or(0, |last| last + 1);
    let body = &token[..end];

    !body.is_empty()
        && body
            .iter()
            .all(|byte| byte.is_ascii_alphanumeric() || b"-._~+/".contains(byte))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read(headers: &[&str]) -> Result<Option<String>> {
        let mut request = http::Request::get("/api/notes");
        for value in headers {
            request = request.header(AUTHORIZATION, *value);
        }
        let (parts, ()) = request.body(()).expect("a valid request").into_parts();

        Bearer::read(&parts)
    }

    #[test]
    fn the_token_is_read_whatever_the_case_of_the_scheme() {
        for value in [
            "Bearer abc.DEF-123",
            "bearer abc.DEF-123",
            "BEARER   abc.DEF-123",
            "Bearer abc.DEF-123 ",
        ] {
            assert_eq!(
                read(&[value]).expect("a token").as_deref(),
                Some("abc.DEF-123"),
                "{value}"
            );
        }
        assert_eq!(
            read(&["Bearer a+b/c~d_e=="]).expect("a token").as_deref(),
            Some("a+b/c~d_e==")
        );
    }

    #[test]
    fn no_header_or_another_scheme_is_no_credential() {
        assert_eq!(read(&[]).expect("no credential"), None);
        assert_eq!(
            read(&["Basic YW5hOnNlY3JldA=="]).expect("no credential"),
            None
        );
        assert_eq!(read(&["Bearerabc"]).expect("no credential"), None);
    }

    #[test]
    fn a_malformed_token_is_a_401_that_does_not_show_it() {
        for value in [
            "Bearer",
            "Bearer ",
            "Bearer secret token",
            "Bearer secret=token",
            "Bearer ==",
            "Bearer secret,token",
        ] {
            let error = read(&[value]).expect_err("not a token");
            let shown = format!("{error} {error:?}");

            assert_eq!(
                error.status(),
                Some(http::StatusCode::UNAUTHORIZED),
                "{value}"
            );
            assert!(!shown.contains("secret"), "{value}: {shown}");
        }
    }

    /// What `Bearer::read` makes of these `Authorization` headers, as bytes.
    fn read_bytes(headers: &[&[u8]]) -> Result<Option<String>> {
        let mut request = http::Request::get("/api/notes")
            .body(())
            .expect("a valid request");
        for value in headers {
            request.headers_mut().append(
                AUTHORIZATION,
                http::HeaderValue::from_bytes(value).expect("a header's value"),
            );
        }
        let (parts, ()) = request.into_parts();

        Bearer::read(&parts)
    }

    #[test]
    fn two_headers_beside_a_bearer_token_are_a_401() {
        for headers in [
            ["Bearer a", "Bearer b"],
            ["Bearer a", "Key b"],
            ["Key b", "Bearer a"],
        ] {
            let error = read(&headers).expect_err("two headers");

            assert_eq!(
                error.status(),
                Some(http::StatusCode::UNAUTHORIZED),
                "{headers:?}"
            );
        }
    }

    #[test]
    fn headers_of_other_schemes_are_left_to_them() {
        assert_eq!(read(&["Basic YW5h", "Key b"]).expect("no credential"), None);
        assert_eq!(
            read_bytes(&[b"Key caf\xe9"]).expect("no credential"),
            None,
            "a header of another scheme that is not ASCII"
        );
    }

    #[test]
    fn a_bearer_token_that_is_not_ascii_is_a_401() {
        let error = read_bytes(&[b"Bearer caf\xe9"]).expect_err("not ASCII");

        assert_eq!(error.status(), Some(http::StatusCode::UNAUTHORIZED));
    }

    #[test]
    fn a_b64token_is_letters_digits_and_six_signs_then_equals() {
        for token in ["a", "A-z.0_9~+/", "abc=", "abc=="] {
            assert!(is_b64token(token.as_bytes()), "{token}");
        }
        for token in ["", "=", "==", "a b", "a=b", "a,b", "é", "a\n"] {
            assert!(!is_b64token(token.as_bytes()), "{token:?}");
        }
    }
}
