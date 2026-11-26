//! The headers of an error: those a handler or a fake gives it, which the server answers with its problem, and those
//! of the answer the client received.

use std::sync::LazyLock;
use std::time::Duration;

use http::header::RETRY_AFTER;
use http::{HeaderMap, HeaderName, HeaderValue};

/// The headers of an error, none until one is given: an error stays two words wide, its headers boxed as its repr is.
#[derive(Default)]
pub(crate) struct Headers(Option<Box<Given>>);

/// The headers given, and the first one refused.
#[derive(Default)]
struct Given {
    map: HeaderMap,
    refused: Option<String>,
}

/// The headers of an error that has none.
static NONE: LazyLock<HeaderMap> = LazyLock::new(HeaderMap::new);

/// The headers quoll writes from the problem it answers: an error does not give them.
const WRITTEN: [&str; 2] = ["content-length", "content-type"];

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

impl Headers {
    /// The headers of the answer the client received.
    pub(crate) fn received(map: HeaderMap) -> Self {
        Self(Some(Box::new(Given { map, refused: None })))
    }

    /// The headers.
    pub(crate) fn map(&self) -> &HeaderMap {
        self.0.as_ref().map_or(&NONE, |given| &given.map)
    }

    /// The first header given that was refused, which says why, `None` when every one was kept.
    pub(crate) fn refused(&self) -> Option<&str> {
        self.0.as_ref().and_then(|given| given.refused.as_deref())
    }

    /// Adds the header `name` with `value`, after those of that name, and returns them; or keeps it out and records
    /// why, when `name` or `value` is not one, or `name` is a header an error does not give.
    pub(crate) fn append<K, V>(&mut self, name: K, value: V) -> Option<(HeaderName, HeaderValue)>
    where
        K: TryInto<HeaderName>,
        V: TryInto<HeaderValue>,
    {
        let header = checked(name, value);
        let given = self.0.get_or_insert_default();

        match header {
            Ok((name, value)) => {
                given.map.append(&name, value.clone());

                Some((name, value))
            }
            Err(refused) => {
                given.refused.get_or_insert(refused);

                None
            }
        }
    }

    /// Sets `Retry-After` to `wait`, in place of any the headers have, and returns its value.
    pub(crate) fn retry_after(&mut self, wait: Duration) -> HeaderValue {
        let value = retry_after(wait);

        self.0
            .get_or_insert_default()
            .map
            .insert(RETRY_AFTER, value.clone());

        value
    }
}

/// The header `name` with `value`, or why an error cannot give it.
fn checked<K, V>(name: K, value: V) -> Result<(HeaderName, HeaderValue), String>
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
fn retry_after(wait: Duration) -> HeaderValue {
    let seconds = wait
        .as_secs()
        .saturating_add(u64::from(wait.subsec_nanos() > 0));

    HeaderValue::from(seconds)
}

#[cfg(test)]
mod tests {
    use http::header::{CONNECTION, CONTENT_TYPE, WWW_AUTHENTICATE};

    use super::*;

    #[test]
    fn a_header_is_appended_after_those_of_its_name() {
        let mut headers = Headers::default();

        headers.append(WWW_AUTHENTICATE, "Bearer");
        headers.append("www-authenticate", "Basic");

        let values: Vec<_> = headers.map().get_all(WWW_AUTHENTICATE).iter().collect();

        assert_eq!(values, ["Bearer", "Basic"]);
        assert_eq!(headers.refused(), None);
    }

    #[test]
    fn a_header_quoll_writes_or_of_a_connection_is_refused_and_the_first_refusal_kept() {
        let mut headers = Headers::default();

        assert!(headers.append(CONTENT_TYPE, "text/html").is_none());
        assert!(headers.append(CONNECTION, "close").is_none());
        assert!(headers.map().is_empty());
        assert_eq!(
            headers.refused(),
            Some("the header `content-type` is written by quoll from the problem it answers")
        );

        let mut connection = Headers::default();

        connection.append("Keep-Alive", "timeout=5");

        assert_eq!(
            connection.refused(),
            Some(
                "the header `keep-alive` is hop-by-hop: it belongs to a connection, not to an answer"
            )
        );
    }

    #[test]
    fn a_name_or_a_value_that_is_not_one_is_refused_without_its_value() {
        let mut name = Headers::default();
        let mut value = Headers::default();

        name.append("a name", "x");
        value.append("x-note", "a\nb");

        assert_eq!(name.refused(), Some("a header's name is not one"));
        assert_eq!(
            value.refused(),
            Some("the value of the header `x-note` is not a header's value")
        );
    }

    #[test]
    fn retry_after_counts_a_part_of_a_second_as_a_whole_and_replaces_the_one_given() {
        let mut headers = Headers::default();

        headers.append(RETRY_AFTER, "120");

        assert_eq!(headers.retry_after(Duration::from_millis(1500)), "2");
        assert_eq!(headers.map().get_all(RETRY_AFTER).iter().count(), 1);
        assert_eq!(retry_after(Duration::from_secs(30)), "30");
        assert_eq!(retry_after(Duration::ZERO), "0");
        assert_eq!(retry_after(Duration::MAX), u64::MAX.to_string().as_str());
    }
}
