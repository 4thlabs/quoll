//! The scheme `Basic`: a user and a password in the `Authorization` header, as RFC 7617 sends them.

use std::fmt;

use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use http::HeaderValue;
use http::request::Parts;

use crate::auth::bearer::authorization;
use crate::openapi::SecurityScheme;
use crate::{Error, Result, Scheme};

/// A user and a password in the `Authorization` header: `Authorization: Basic <base64 of user:password>`, as RFC 7617
/// sends them. Its credential is a [`BasicCredential`].
///
/// The scheme's name is read whatever its case, and the user and the password are UTF-8. A 401 on a route that takes
/// it carries `WWW-Authenticate: Basic realm="api", charset="UTF-8"`, which makes a browser open its own login window:
/// a route that the web page calls had better take another scheme.
///
/// ```
/// use quoll::{Basic, Scheme};
///
/// let request = http::Request::get("/api/stats")
///     .header("Authorization", "Basic YW5hOnNlY3JldA==")
///     .body(())
///     .unwrap();
/// let (parts, ()) = request.into_parts();
/// let credential = Basic::read(&parts).unwrap().unwrap();
///
/// assert_eq!(credential.user, "ana");
/// assert_eq!(credential.password, "secret");
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Basic;

/// The credential of [`Basic`]: a user and a password. Its `Debug` does not show the password.
#[derive(Clone, PartialEq, Eq)]
pub struct BasicCredential {
    /// The user, which holds no `:`.
    pub user: String,
    /// The password.
    pub password: String,
}

impl fmt::Debug for BasicCredential {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("BasicCredential")
            .field("user", &self.user)
            .field("password", &"<hidden>")
            .finish()
    }
}

impl Scheme for Basic {
    type Credential = BasicCredential;

    const NAME: &'static str = "Basic";

    const CHALLENGE: HeaderValue =
        HeaderValue::from_static(r#"Basic realm="api", charset="UTF-8""#);

    /// The user and the password of the request's `Authorization: Basic` header. `None` when the request has no
    /// `Authorization` header of the scheme `Basic`: none at all, or only headers of another scheme, which this one
    /// leaves to the scheme they are for.
    ///
    /// # Errors
    ///
    /// A 401, whose detail never shows the header, when the request has an `Authorization: Basic` header and:
    ///
    /// - another `Authorization` header beside it;
    /// - a value that is not base64;
    /// - a value that is not UTF-8 once decoded, has no `:` between the user and the password, or holds a control
    ///   character.
    fn read(request: &Parts) -> Result<Option<BasicCredential>> {
        let Some(encoded) = authorization(request, "Basic")? else {
            return Ok(None);
        };

        let decoded = STANDARD.decode(encoded).map_err(|_| {
            Error::unauthorized(
                "the basic credential is not well formed: it is the base64 of `user:password`",
            )
        })?;
        let text = String::from_utf8(decoded)
            .map_err(|_| Error::unauthorized("the basic credential is not UTF-8"))?;

        if text.chars().any(char::is_control) {
            return Err(Error::unauthorized(
                "the basic credential holds a control character",
            ));
        }

        let Some((user, password)) = text.split_once(':') else {
            return Err(Error::unauthorized(
                "the basic credential has no `:` between the user and the password",
            ));
        };

        Ok(Some(BasicCredential {
            user: user.to_owned(),
            password: password.to_owned(),
        }))
    }

    /// `{type: http, scheme: basic}`.
    fn openapi() -> SecurityScheme {
        SecurityScheme::basic()
    }
}

#[cfg(test)]
mod tests {
    use http::header::AUTHORIZATION;

    use super::*;

    fn read(headers: &[&str]) -> Result<Option<BasicCredential>> {
        let mut request = http::Request::get("/api/stats");
        for value in headers {
            request = request.header(AUTHORIZATION, *value);
        }
        let (parts, ()) = request.body(()).expect("a valid request").into_parts();

        Basic::read(&parts)
    }

    fn encoded(text: &[u8]) -> String {
        format!("Basic {}", STANDARD.encode(text))
    }

    #[test]
    fn the_user_and_the_password_are_read_whatever_the_case_of_the_scheme() {
        for scheme in ["Basic", "basic", "BASIC"] {
            let value = format!("{scheme} {}", STANDARD.encode("ana:se:cret"));
            let credential = read(&[&value]).expect("a credential").expect("present");

            assert_eq!(credential.user, "ana", "{scheme}");
            assert_eq!(credential.password, "se:cret", "{scheme}");
        }

        let credential = read(&[&encoded("zoé:".as_bytes())])
            .expect("a credential")
            .expect("present");
        assert_eq!(
            (credential.user.as_str(), credential.password.as_str()),
            ("zoé", "")
        );
    }

    #[test]
    fn no_header_or_another_scheme_is_no_credential() {
        assert_eq!(read(&[]).expect("no credential"), None);
        assert_eq!(read(&["Bearer abc"]).expect("no credential"), None);
        assert_eq!(read(&["Basicabc"]).expect("no credential"), None);
    }

    #[test]
    fn a_malformed_credential_is_a_401_that_does_not_show_it() {
        let cases = [
            "Basic".to_owned(),
            "Basic not*base64".to_owned(),
            encoded(b"secret-without-colon"),
            encoded(b"ana:\xffsecret"),
            encoded(b"ana:sec\nret"),
        ];

        for value in &cases {
            let error = read(&[value]).expect_err("not a credential");
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
    fn a_basic_header_beside_another_is_a_401() {
        let basic = encoded(b"ana:secret");

        for headers in [[basic.as_str(), "Bearer b"], ["Bearer b", basic.as_str()]] {
            let error = read(&headers).expect_err("two headers");

            assert_eq!(error.status(), Some(http::StatusCode::UNAUTHORIZED));
        }
    }

    #[test]
    fn the_debug_of_a_credential_hides_its_password() {
        let credential = BasicCredential {
            user: "ana".into(),
            password: "secret".into(),
        };
        let shown = format!("{credential:?}");

        assert!(shown.contains("ana"), "{shown}");
        assert!(!shown.contains("secret"), "{shown}");
    }
}
