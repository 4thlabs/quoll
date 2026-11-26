//! The grammar of `Authorization` as a server reads it, RFC 9110 11.4: `credentials = auth-scheme [ 1*SP ( token68 /
//! #auth-param ) ]`, and the checks of its parts, which a client writes with [`Outgoing`](super::Outgoing).

use std::fmt;

use http::header::AUTHORIZATION;
use http::request::Parts;

use crate::{Error, Result};

/// What follows the auth-scheme `scheme` in the request's `Authorization` header: `None` when the request has no
/// `Authorization` header of that auth-scheme, compared without case, none at all or only headers of another, which
/// are left to the scheme they are for.
///
/// ```
/// let request = http::Request::get("/Items")
///     .header("Authorization", r#"MediaBrowser Client="web", Token="k-1""#)
///     .body(())
///     .unwrap();
/// let (parts, ()) = request.into_parts();
/// let credentials = quoll::auth::authorization(&parts, "MediaBrowser").unwrap().unwrap();
///
/// assert_eq!(credentials.param("Token").unwrap().as_deref(), Some("k-1"));
/// ```
///
/// # Errors
///
/// A 401 when the header of the auth-scheme has another `Authorization` header beside it, which leaves it unclear
/// what the request authenticates with.
pub fn authorization<'a>(request: &'a Parts, scheme: &'a str) -> Result<Option<Credentials<'a>>> {
    let headers = request.headers.get_all(AUTHORIZATION);
    let Some(rest) = headers
        .iter()
        .find_map(|value| after_scheme(value.as_bytes(), scheme))
    else {
        return Ok(None);
    };

    if headers.iter().nth(1).is_some() {
        return Err(Error::unauthorized(format!(
            "the request has an `Authorization: {scheme}` header and another `Authorization` header"
        )));
    }

    Ok(Some(Credentials {
        scheme,
        rest: rest.trim_ascii(),
    }))
}

/// What follows the auth-scheme of an `Authorization` header that [`authorization`] found: a token68, or a list of
/// auth-params. Neither its errors nor its `Debug` show the credential.
#[derive(Clone, Copy)]
pub struct Credentials<'a> {
    scheme: &'a str,
    rest: &'a [u8],
}

impl fmt::Debug for Credentials<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Credentials")
            .field("scheme", &self.scheme)
            .finish_non_exhaustive()
    }
}

impl<'a> Credentials<'a> {
    /// The bytes after the auth-scheme, trimmed, as they are.
    pub fn as_bytes(&self) -> &'a [u8] {
        self.rest
    }

    /// The credential as a `token68`: letters, digits, `-._~+/`, and `=` at its end.
    ///
    /// # Errors
    ///
    /// A 401 when it is empty, or not a `token68`.
    pub fn token68(&self) -> Result<&'a str> {
        let scheme = self.scheme;

        if self.rest.is_empty() {
            return Err(Error::unauthorized(format!(
                "the `{scheme}` credential is empty"
            )));
        }

        if !is_token68(self.rest) {
            return Err(Error::unauthorized(format!(
                "the `{scheme}` credential is not well formed: it is letters, digits, `-._~+/`, and `=` at its end"
            )));
        }

        // A token68 is ASCII.
        Ok(std::str::from_utf8(self.rest).expect("a token68 is ASCII"))
    }

    /// The value of the auth-param `name`, its name compared without case and a quoted-string unescaped: `None` when
    /// the list has no such param. The other params are left as they are.
    ///
    /// # Errors
    ///
    /// A 401 when the list is not a list of `name=token` or `name="quoted"`, or holds `name` twice.
    pub fn param(&self, name: &str) -> Result<Option<String>> {
        let scheme = self.scheme;
        let params = parse_params(self.rest).ok_or_else(|| {
            Error::unauthorized(format!(
                "the `{scheme}` credentials are not a list of `name=value` or `name=\"value\"`"
            ))
        })?;
        let mut found = params
            .into_iter()
            .filter(|(key, _)| key.eq_ignore_ascii_case(name.as_bytes()));
        let value = found.next();

        if found.next().is_some() {
            return Err(Error::unauthorized(format!(
                "the `{scheme}` credentials have `{name}` twice"
            )));
        }

        Ok(value.map(|(_, value)| value))
    }
}

/// What follows the auth-scheme `scheme` in an `Authorization` value, whatever the case of its name; `None` for a value
/// of another auth-scheme.
fn after_scheme<'a>(value: &'a [u8], scheme: &str) -> Option<&'a [u8]> {
    let (name, rest) = match value.iter().position(|&byte| byte == b' ') {
        Some(space) => (&value[..space], &value[space + 1..]),
        None => (value, &[][..]),
    };

    name.eq_ignore_ascii_case(scheme.as_bytes()).then_some(rest)
}

/// Whether `text` is an RFC 9110 `token`: one or more ASCII letters, digits and ``!#$%&'*+-.^_`|~``.
pub fn is_token(text: impl AsRef<[u8]>) -> bool {
    let text = text.as_ref();

    !text.is_empty() && text.iter().all(|&byte| is_tchar(byte))
}

/// Whether `text` is an RFC 9110 `token68`, RFC 6750's `b64token`: `1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" /
/// "/" ) *"="`.
pub fn is_token68(text: impl AsRef<[u8]>) -> bool {
    let text = text.as_ref();
    let end = text
        .iter()
        .rposition(|&byte| byte != b'=')
        .map_or(0, |last| last + 1);
    let body = &text[..end];

    !body.is_empty()
        && body
            .iter()
            .all(|byte| byte.is_ascii_alphanumeric() || b"-._~+/".contains(byte))
}

fn is_tchar(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&byte)
}

/// The auth-params of `text`, `#auth-param`: `None` when it is not such a list.
fn parse_params(text: &[u8]) -> Option<Vec<(&[u8], String)>> {
    let mut params = Vec::new();
    let mut rest = text;

    loop {
        rest = skip_blanks_and_commas(rest);
        if rest.is_empty() {
            return Some(params);
        }

        let end = rest
            .iter()
            .position(|&byte| !is_tchar(byte))
            .unwrap_or(rest.len());
        let (name, after) = rest.split_at(end);
        if name.is_empty() {
            return None;
        }

        let after = after.trim_ascii_start();
        let after = after.strip_prefix(b"=")?.trim_ascii_start();
        let (value, after) = if let Some(quoted) = after.strip_prefix(b"\"") {
            quoted_string(quoted)?
        } else {
            let end = after
                .iter()
                .position(|&byte| !is_tchar(byte))
                .unwrap_or(after.len());
            if end == 0 {
                return None;
            }
            let value = std::str::from_utf8(&after[..end]).ok()?.to_owned();
            (value, &after[end..])
        };

        params.push((name, value));

        let after = after.trim_ascii_start();
        if !after.is_empty() && !after.starts_with(b",") {
            return None;
        }
        rest = after;
    }
}

fn skip_blanks_and_commas(mut text: &[u8]) -> &[u8] {
    while let [b' ' | b'\t' | b',', rest @ ..] = text {
        text = rest;
    }
    text
}

/// The value of a quoted-string whose opening `"` is read, unescaped, and what follows its closing `"`.
fn quoted_string(text: &[u8]) -> Option<(String, &[u8])> {
    let mut value = Vec::new();
    let mut i = 0;

    while i < text.len() {
        match text[i] {
            b'"' => return Some((String::from_utf8(value).ok()?, &text[i + 1..])),
            b'\\' => {
                value.push(*text.get(i + 1)?);
                i += 2;
            }
            byte => {
                value.push(byte);
                i += 1;
            }
        }
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn param(value: &str, name: &str) -> Result<Option<String>> {
        let (parts, ()) = http::Request::get("/")
            .header(AUTHORIZATION, value)
            .body(())
            .expect("a request")
            .into_parts();

        authorization(&parts, "MediaBrowser")?
            .expect("of the scheme")
            .param(name)
    }

    #[test]
    fn a_param_is_read_whatever_its_case_its_blanks_and_the_others() {
        for value in [
            r#"MediaBrowser Token="k-1""#,
            r#"mediabrowser token="k-1""#,
            r#"MediaBrowser   Token = "k-1""#,
            r#"MediaBrowser Token=k-1"#,
            r#"MediaBrowser Client="Jellyfin Web", Device="Firefox", Token="k-1", Version="10.11.0""#,
        ] {
            assert_eq!(
                param(value, "Token").expect(value).as_deref(),
                Some("k-1"),
                "{value}"
            );
        }

        assert_eq!(
            param(r#"MediaBrowser Token="k-\"1""#, "Token")
                .expect("escaped")
                .as_deref(),
            Some("k-\"1")
        );
        assert_eq!(
            param(r#"MediaBrowser Client="web""#, "Token").expect("none"),
            None
        );
        assert!(param(r#"MediaBrowser Token="a", Token="b""#, "Token").is_err());
        assert!(param(r#"MediaBrowser Token="k-1"#, "Token").is_err());
        assert!(param(r#"MediaBrowser Token"#, "Token").is_err());
    }
}
