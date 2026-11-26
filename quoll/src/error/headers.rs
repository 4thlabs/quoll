//! The headers an error may be given, which the server answers with its problem, and the value of a `Retry-After`.

use std::time::Duration;

use http::{HeaderName, HeaderValue};

/// The headers that describe the body of the problem quoll writes: an error does not give them.
const WRITTEN: [&str; 4] = [
    "content-encoding",
    "content-length",
    "content-range",
    "content-type",
];

/// The hop-by-hop headers, of RFC 9110's section 7.6.1, which say how a connection carries an answer and belong to
/// that connection alone: an error does not give them.
const HOP_BY_HOP: [&str; 7] = [
    "connection",
    "keep-alive",
    "proxy-connection",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
];

/// The header `name` with `value`, or why an error cannot give it, which names it and never shows its value.
pub(crate) fn checked<K, V>(name: K, value: V) -> Result<(HeaderName, HeaderValue), String>
where
    K: TryInto<HeaderName>,
    V: TryInto<HeaderValue>,
{
    let Ok(name) = name.try_into() else {
        return Err("a header's name is not one".to_owned());
    };

    if WRITTEN.contains(&name.as_str()) {
        return Err(format!(
            "the header `{name}` is written by quoll from the problem it answers"
        ));
    }

    if HOP_BY_HOP.contains(&name.as_str()) {
        return Err(format!(
            "the header `{name}` is hop-by-hop: it belongs to a connection, not to an answer"
        ));
    }

    let Ok(value) = value.try_into() else {
        return Err(format!(
            "the value of the header `{name}` is not a header's value"
        ));
    };

    Ok((name, value))
}

/// The value of a `Retry-After` of `wait`: its seconds, a part of one counted as a whole, so that a client never
/// comes back before the wait has passed, and `u64::MAX` of them at most.
pub(crate) fn retry_after(wait: Duration) -> HeaderValue {
    let seconds = wait
        .as_secs()
        .saturating_add(u64::from(wait.subsec_nanos() > 0));

    HeaderValue::from(seconds)
}

#[cfg(test)]
mod tests {
    use http::header::{
        CONNECTION, CONTENT_ENCODING, CONTENT_LENGTH, CONTENT_RANGE, CONTENT_TYPE, WWW_AUTHENTICATE,
    };

    use super::*;

    #[test]
    fn a_header_an_answer_carries_is_kept() {
        let (name, value) = checked(WWW_AUTHENTICATE, "Bearer").expect("a header");

        assert_eq!(name, WWW_AUTHENTICATE);
        assert_eq!(value, "Bearer");
        assert!(checked("Retry-After", 30).is_ok());
    }

    #[test]
    fn a_header_of_the_body_quoll_writes_is_refused() {
        for name in [
            CONTENT_TYPE,
            CONTENT_LENGTH,
            CONTENT_ENCODING,
            CONTENT_RANGE,
        ] {
            assert_eq!(
                checked(name.clone(), "x").expect_err("refused"),
                format!("the header `{name}` is written by quoll from the problem it answers")
            );
        }
    }

    #[test]
    fn a_header_of_a_connection_is_refused() {
        assert_eq!(
            checked(CONNECTION, "close").expect_err("refused"),
            "the header `connection` is hop-by-hop: it belongs to a connection, not to an answer"
        );
        assert_eq!(
            checked("Keep-Alive", "timeout=5").expect_err("refused"),
            "the header `keep-alive` is hop-by-hop: it belongs to a connection, not to an answer"
        );
    }

    #[test]
    fn a_name_or_a_value_that_is_not_one_is_refused_without_its_value() {
        assert_eq!(
            checked("a name", "x").expect_err("refused"),
            "a header's name is not one"
        );
        assert_eq!(
            checked("x-note", "a\nb").expect_err("refused"),
            "the value of the header `x-note` is not a header's value"
        );
    }

    #[test]
    fn retry_after_counts_a_part_of_a_second_as_a_whole() {
        assert_eq!(retry_after(Duration::from_millis(1500)), "2");
        assert_eq!(retry_after(Duration::from_secs(30)), "30");
        assert_eq!(retry_after(Duration::ZERO), "0");
        assert_eq!(retry_after(Duration::MAX), u64::MAX.to_string().as_str());
    }
}
