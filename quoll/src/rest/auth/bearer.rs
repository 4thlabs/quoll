//! The scheme `Bearer`: a token in the `Authorization` header, as RFC 6750 sends it.

use http::header::AUTHORIZATION;
use http::request::Parts;

use crate::rest::{Error, Result, Scheme};

/// A token in the `Authorization` header: `Authorization: Bearer <token>`, as RFC 6750 sends it. Its credential is the
/// token, a `String`.
///
/// The scheme's name is read whatever its case, `bearer` as `Bearer`, and the token is an RFC 6750 `b64token`:
/// letters, digits, `-._~+/`, and `=` at its end.
///
/// ```
/// use quoll::rest::{Bearer, Scheme};
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

impl Bearer {
    /// The scheme's name, which `rest::Route::schemes` holds for a route that takes it.
    #[cfg_attr(
        not(feature = "client"),
        expect(dead_code, reason = "only the client's middleware reads it")
    )]
    pub(crate) const NAME: &str = "Bearer";
}

impl Scheme for Bearer {
    type Credential = String;

    /// The token of the request's `Authorization: Bearer` header. `None` when the request has no `Authorization`
    /// header, or one of another scheme.
    ///
    /// # Errors
    ///
    /// A 401 when the request has two `Authorization` headers, or a `Bearer` one whose token is empty or not a
    /// `b64token`. Its detail never shows the header.
    fn read(request: &Parts) -> Result<Option<String>> {
        let mut values = request.headers.get_all(AUTHORIZATION).iter();
        let Some(value) = values.next() else {
            return Ok(None);
        };
        if values.next().is_some() {
            return Err(Error::unauthorized(
                "the request has more than one `Authorization` header",
            ));
        }

        // A header that is not text is no credential of this scheme's, which is ASCII.
        let Ok(value) = value.to_str() else {
            return Err(Error::unauthorized(
                "the `Authorization` header is not ASCII",
            ));
        };
        let (scheme, token) = value.split_once(' ').unwrap_or((value, ""));
        if !scheme.eq_ignore_ascii_case("Bearer") {
            return Ok(None);
        }

        let token = token.trim_matches([' ', '\t']);
        if token.is_empty() {
            return Err(Error::unauthorized("the bearer token is empty"));
        }
        if !is_b64token(token) {
            return Err(Error::unauthorized(
                "the bearer token is not well formed: it is letters, digits, `-._~+/`, and `=` at its end",
            ));
        }

        Ok(Some(token.to_owned()))
    }
}

/// Whether `token` is an RFC 6750 `b64token`: `1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" ) *"="`.
fn is_b64token(token: &str) -> bool {
    let body = token.trim_end_matches('=');

    !body.is_empty()
        && body
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"-._~+/".contains(&byte))
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

    #[test]
    fn two_headers_are_a_401() {
        let error = read(&["Bearer a", "Bearer b"]).expect_err("two headers");

        assert_eq!(error.status(), Some(http::StatusCode::UNAUTHORIZED));
    }

    #[test]
    fn a_header_that_is_not_ascii_is_a_401() {
        let mut request = http::Request::get("/api/notes")
            .body(())
            .expect("a valid request");
        request.headers_mut().insert(
            AUTHORIZATION,
            http::HeaderValue::from_bytes(b"Bearer caf\xe9").expect("a header's value"),
        );
        let (parts, ()) = request.into_parts();

        let error = Bearer::read(&parts).expect_err("not ASCII");

        assert_eq!(error.status(), Some(http::StatusCode::UNAUTHORIZED));
    }
}
