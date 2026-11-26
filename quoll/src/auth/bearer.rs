//! The scheme `Bearer`: a token in the `Authorization` header, as RFC 6750 sends it.

use http::request::Parts;

use crate::auth::{Outgoing, Unsendable, authorization, is_token68};
use crate::openapi::SecurityScheme;
use crate::{ClientScheme, Error, Result, Scheme, ServerScheme};

/// A token in the `Authorization` header: `Authorization: Bearer <token>`, as RFC 6750 sends it. Its credential is the
/// token, a `String`.
///
/// The scheme's name is read whatever its case, `bearer` as `Bearer`, and the token is an RFC 6750 `b64token`:
/// letters, digits, `-._~+/`, and `=` at its end.
///
/// ```
/// use quoll::{Bearer, ServerScheme};
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
    const NAME: &'static str = "Bearer";
}

impl ClientScheme for Bearer {
    type Secret = String;

    /// `Authorization: Bearer <token>`.
    ///
    /// # Errors
    ///
    /// When the token is not a `b64token`, letters, digits, `-._~+/` and `=` at its end, which no server of a contract
    /// reads.
    fn write(token: &String, request: &mut Outgoing<'_>) -> std::result::Result<(), Unsendable> {
        request.authorization_token68("Bearer", token).map_err(|_| {
            Unsendable::new(
                "the bearer token cannot be sent: it is not a `b64token`, letters, digits, `-._~+/` and `=` at its end",
            )
        })
    }
}

impl ServerScheme for Bearer {
    type Credential = String;

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
        let Some(credentials) = authorization(request, "Bearer")? else {
            return Ok(None);
        };
        let token = credentials.as_bytes();

        if token.is_empty() {
            return Err(Error::unauthorized("the bearer token is empty"));
        }

        if !is_token68(token) {
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

#[cfg(test)]
mod tests {
    use http::header::AUTHORIZATION;

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
            assert!(is_token68(token.as_bytes()), "{token}");
        }
        for token in ["", "=", "==", "a b", "a=b", "a,b", "é", "a\n"] {
            assert!(!is_token68(token.as_bytes()), "{token:?}");
        }
    }
}
