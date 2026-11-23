//! The content type of a body, as the server reads a request's and the client an answer's, and as a `Bytes` carries
//! it.
//!
//! A message with more than one `Content-Type` has none that is read: each check here refuses it.

// `Bytes` reads its default type and the essence of its own whatever the features, the rest only a side does.
#![cfg_attr(
    not(any(feature = "server", feature = "client")),
    expect(
        dead_code,
        reason = "with neither side, only `Bytes` reads a content type"
    )
)]

use std::borrow::Cow;

use http::{HeaderMap, HeaderValue, header};

use crate::__private::headers;

/// A JSON body.
pub const JSON: &str = "application/json";

/// A problem, RFC 9457, in JSON.
pub const PROBLEM: &str = "application/problem+json";

/// A form.
pub const FORM: &str = "application/x-www-form-urlencoded";

/// A form with files.
pub const MULTIPART: &str = "multipart/form-data";

/// Text, read in UTF-8 when no charset is named.
pub const TEXT: &str = "text/plain";

/// Text as quoll sends it, which names its charset.
pub const TEXT_UTF8: &str = "text/plain; charset=utf-8";

/// Server-sent events.
#[cfg_attr(
    not(feature = "client"),
    expect(dead_code, reason = "on the server, axum's `Sse` writes it")
)]
pub const EVENT_STREAM: &str = "text/event-stream";

/// JSON Lines, a JSON value per line.
pub const JSON_LINES: &str = "application/jsonl";

/// Bytes of no known type, what a `Bytes` is unless it says otherwise.
pub const OCTET_STREAM: &str = "application/octet-stream";

/// The one `Content-Type` of `headers`, as text: none when it is missing, given more than once, or not text.
fn single(headers: &HeaderMap) -> Option<&str> {
    let mut values = headers.get_all(header::CONTENT_TYPE).iter();
    let (Some(value), None) = (values.next(), values.next()) else {
        return None;
    };

    value.to_str().ok()
}

/// The type and the subtype of `content_type`, without its parameters, as it is written: what is compared to a type,
/// whatever the case.
pub(crate) fn essence(content_type: &str) -> &str {
    content_type.split(';').next().unwrap_or_default().trim()
}

/// Whether `headers` say the body is `expected`, whatever the case and the parameters, but a charset other than UTF-8
/// when `utf8` is set. A body without a content type, with more than one, or with one that is not text, is not
/// `expected`.
pub(crate) fn is(headers: &HeaderMap, expected: &str, utf8: bool) -> bool {
    let Some(value) = single(headers) else {
        return false;
    };

    if !essence(value).eq_ignore_ascii_case(expected) {
        return false;
    }

    let other_charset = params(value.as_bytes()).any(|param| {
        let Some(at) = param.iter().position(|&byte| byte == b'=') else {
            return false;
        };
        let (name, value) = param.split_at(at);
        let value = headers::unquoted(value[1..].trim_ascii());

        name.trim_ascii().eq_ignore_ascii_case(b"charset")
            && value.is_none_or(|value| !value.eq_ignore_ascii_case(b"utf-8"))
    });

    !(utf8 && other_charset)
}

/// The parameters of a content type, each cut at a `;` outside a quoted string, as the macro reads a fixed one:
/// `profile="a;b"` is one.
fn params(value: &[u8]) -> impl Iterator<Item = &[u8]> {
    let (_, mut rest) = headers::split_param(value);

    std::iter::from_fn(move || {
        let (param, after) = headers::split_param(rest?);

        rest = after;
        Some(param)
    })
}

/// The one content type of `headers`, when it is a media type: none when it is missing, given more than once, or not
/// a media type.
pub(crate) fn media_type(headers: &HeaderMap) -> Option<&str> {
    single(headers).filter(|value| is_media_type(value))
}

/// `content_type` as a header's value, when it is a media type; given back otherwise, for the error to say.
pub(crate) fn media_type_value(
    content_type: Cow<'static, str>,
) -> Result<HeaderValue, Cow<'static, str>> {
    match HeaderValue::from_str(&content_type) {
        Ok(value) if is_media_type(&content_type) => Ok(value),
        _ => Err(content_type),
    }
}

/// Whether a browser sends `media_type` to another site without asking it first, with no CORS preflight: a form, a
/// form with files and text, whatever their case and parameters.
#[cfg_attr(
    not(any(test, feature = "server")),
    expect(dead_code, reason = "only the server reads a body")
)]
pub(crate) fn is_cors_safelisted(media_type: &str) -> bool {
    let essence = essence(media_type);

    [FORM, MULTIPART, TEXT]
        .iter()
        .any(|safelisted| essence.eq_ignore_ascii_case(safelisted))
}

/// Whether `value` is a media type as RFC 9110 writes one: `type/subtype`, each a token, then parameters, each `;`
/// and `name=value`, the name a token and the value a token or a quoted string, with spaces or tabs around the `;`
/// only. It is ASCII: what a header's value holds as text.
pub(crate) fn is_media_type(value: &str) -> bool {
    let Some(mut rest) = token(value.as_bytes())
        .and_then(|rest| rest.strip_prefix(b"/"))
        .and_then(token)
    else {
        return false;
    };

    loop {
        if rest.is_empty() {
            return true;
        }

        let Some(after) = blanks(rest).strip_prefix(b";") else {
            return false;
        };

        rest = blanks(after);

        if rest.first().is_some_and(|&byte| is_tchar(byte)) {
            let Some(after) = token(rest).and_then(|rest| rest.strip_prefix(b"=")) else {
                return false;
            };
            let Some(after) = token(after).or_else(|| quoted_string(after)) else {
                return false;
            };

            rest = after;
        }
    }
}

/// What follows the token `input` starts with, when it starts with one.
fn token(input: &[u8]) -> Option<&[u8]> {
    let len = input.iter().take_while(|&&byte| is_tchar(byte)).count();

    (len > 0).then(|| &input[len..])
}

/// What follows the quoted string `input` starts with, when it starts with one: printable ASCII, spaces and tabs,
/// with `"` and `\` escaped by a `\`.
fn quoted_string(input: &[u8]) -> Option<&[u8]> {
    let is_text = |byte: u8| byte == b'\t' || (b' '..=b'~').contains(&byte);
    let mut rest = input.strip_prefix(b"\"")?;

    loop {
        match rest.split_first()? {
            (b'"', after) => return Some(after),
            (b'\\', after) => {
                let (&escaped, after) = after.split_first()?;

                if !is_text(escaped) {
                    return None;
                }

                rest = after;
            }
            (&byte, after) if is_text(byte) => rest = after,
            _ => return None,
        }
    }
}

/// `input` without the spaces and tabs it starts with.
fn blanks(input: &[u8]) -> &[u8] {
    let len = input
        .iter()
        .take_while(|&&byte| byte == b' ' || byte == b'\t')
        .count();

    &input[len..]
}

/// Whether `byte` may be in a token, RFC 9110.
fn is_tchar(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&byte)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn headers(content_type: &str) -> HeaderMap {
        let mut headers = HeaderMap::new();
        headers.insert(
            header::CONTENT_TYPE,
            HeaderValue::from_str(content_type).expect("a header's value"),
        );

        headers
    }

    #[test]
    fn the_type_is_read_whatever_its_case_and_parameters() {
        assert!(is(&headers("text/plain"), "text/plain", true));
        assert!(is(
            &headers("Text/Plain; charset=UTF-8"),
            "text/plain",
            true
        ));
        assert!(is(
            &headers("text/plain;charset=\"utf-8\""),
            "text/plain",
            true
        ));
        assert!(is(
            &headers("text/plain; format=flowed"),
            "text/plain",
            true
        ));
        assert!(is(
            &headers("application/octet-stream; x=y"),
            "application/octet-stream",
            false
        ));
    }

    #[test]
    fn the_essence_is_the_type_and_subtype_as_written() {
        assert_eq!(essence("Image/PNG; name=\"a.png\""), "Image/PNG");
        assert_eq!(essence(" text/plain ;charset=utf-8"), "text/plain");
        assert_eq!(essence("image/png"), "image/png");
    }

    #[test]
    fn more_than_one_content_type_is_none_of_them() {
        let mut twice = headers("text/plain");
        twice.append(header::CONTENT_TYPE, HeaderValue::from_static("text/plain"));

        assert!(!is(&twice, "text/plain", true));
        assert!(!is(&twice, "text/plain", false));
    }

    #[test]
    fn another_type_or_none_is_not_it() {
        assert!(!is(&headers("text/html"), "text/plain", true));
        assert!(!is(&headers("text/plainx"), "text/plain", true));
        assert!(!is(
            &headers("application/json"),
            "application/octet-stream",
            false
        ));
        assert!(!is(&HeaderMap::new(), "text/plain", true));
    }

    #[test]
    fn another_charset_is_refused_only_for_text() {
        assert!(!is(
            &headers("text/plain; charset=iso-8859-1"),
            "text/plain",
            true
        ));
        assert!(is(
            &headers("text/plain; charset=iso-8859-1"),
            "text/plain",
            false
        ));
    }

    #[test]
    fn a_parameter_is_read_as_the_macro_reads_a_fixed_one() {
        // A `;` in a quoted string cuts no parameter: the charset is the string's.
        assert!(is(
            &headers(r#"application/json; profile="a;charset=latin1""#),
            "application/json",
            true
        ));
        assert!(!is(
            &headers(r#"text/plain; note="a;b"; charset=latin1"#),
            "text/plain",
            true
        ));
        // A charset that is not a token or a closed quoted string is not UTF-8.
        assert!(!is(
            &headers(r#"text/plain; charset="utf-8\""#),
            "text/plain",
            true
        ));
        assert!(!is(
            &headers(r#"text/plain; charset=utf-8""#),
            "text/plain",
            true
        ));
    }

    #[test]
    fn a_media_type_is_a_type_a_subtype_and_parameters() {
        for value in [
            "image/png",
            "application/octet-stream",
            "Image/PNG",
            "application/vnd.api+json",
            "text/csv; charset=utf-8",
            "text/csv;charset=utf-8",
            "text/csv ;\tcharset=utf-8 ; header=present",
            "multipart/mixed; boundary=\"a b; c\"",
            "text/plain; title=\"say \\\"hi\\\"\"",
            "image/png;",
            "image/png; ;",
        ] {
            assert!(is_media_type(value), "{value:?}");
        }
    }

    #[test]
    fn what_is_not_a_media_type_is_refused() {
        for value in [
            "",
            "png",
            "image/",
            "/png",
            "image/png/x",
            " image/png",
            "image/png ",
            "image /png",
            "image/ png",
            "image/png, text/plain",
            "image/png; charset",
            "image/png; charset=",
            "image/png; =utf-8",
            "image/png; a=b c",
            "image/png; a=\"unclosed",
            "image/png; a=\"bad\\\u{7f}\"",
            "image/pngé",
            "image/png; name=\"é\"",
            "image/png\r\nx-y: z",
            "image/png\0",
        ] {
            assert!(!is_media_type(value), "{value:?}");
        }
    }

    #[test]
    fn the_types_a_browser_sends_to_another_site_without_asking_are_known() {
        for value in [
            "text/plain",
            "application/x-www-form-urlencoded",
            "multipart/form-data",
            "Text/Plain; charset=utf-8",
            "MULTIPART/FORM-DATA; boundary=x",
            "application/X-WWW-Form-Urlencoded ;charset=utf-8",
        ] {
            assert!(is_cors_safelisted(value), "{value:?}");
        }

        for value in [
            "application/octet-stream",
            "text/plainx",
            "text/html",
            "multipart/mixed",
            "image/png",
        ] {
            assert!(!is_cors_safelisted(value), "{value:?}");
        }
    }

    #[test]
    fn the_media_type_of_headers_is_their_one_content_type() {
        assert_eq!(media_type(&headers("image/png")), Some("image/png"));
        assert_eq!(media_type(&headers("png")), None);
        assert_eq!(media_type(&HeaderMap::new()), None);

        let mut twice = headers("image/png");
        twice.append(header::CONTENT_TYPE, HeaderValue::from_static("image/png"));
        assert_eq!(media_type(&twice), None);

        let mut not_text = HeaderMap::new();
        not_text.insert(
            header::CONTENT_TYPE,
            HeaderValue::from_bytes(b"image/png; name=\"\xe9\"").expect("a header's value"),
        );
        assert_eq!(media_type(&not_text), None);
    }
}
