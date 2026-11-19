//! The content type of a body, as the server reads a request's and the client an answer's.

use http::{HeaderMap, header};

/// Whether `headers` say the body is `expected`, whatever the case and the parameters, but a charset other than UTF-8
/// when `utf8` is set. A body without a content type, or with one that is not text, is not `expected`.
pub(crate) fn is(headers: &HeaderMap, expected: &str, utf8: bool) -> bool {
    let Some(value) = headers
        .get(header::CONTENT_TYPE)
        .and_then(|value| value.to_str().ok())
    else {
        return false;
    };

    let mut parts = value.split(';');
    let essence = parts.next().unwrap_or_default().trim();

    if !essence.eq_ignore_ascii_case(expected) {
        return false;
    }

    let other_charset = parts
        .filter_map(|param| param.split_once('='))
        .any(|(name, value)| {
            let value = value.trim().trim_matches('"');

            name.trim().eq_ignore_ascii_case("charset") && !value.eq_ignore_ascii_case("utf-8")
        });

    !(utf8 && other_charset)
}

#[cfg(test)]
mod tests {
    use http::HeaderValue;

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
}
