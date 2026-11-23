//! The fixed headers of a contract, `#[header("name" = value)]` on an api, a group or a route: what the generated
//! code names them by, whatever the features, and the checks of their values at compile time, which a constant's
//! value reaches only there.

/// A fixed header: its name, as the contract writes it, and its value, a literal or a constant's, which the
/// generated code has checked at compile time with [`is_value`].
#[derive(Debug, Clone, Copy)]
pub struct Fixed {
    /// Its name, `X-GitHub-Api-Version`: a token, which the macro checks.
    pub name: &'static str,
    /// Its value, `2022-11-28`.
    pub value: &'static str,
}

/// What a route takes of the fixed headers, beside the ones of its api: its own and its group's, and what it leaves
/// of the api's.
#[derive(Debug, Clone, Copy)]
pub struct RouteHeaders {
    /// Its own headers and its group's, the route's winning, as the macro resolves them.
    pub own: &'static [Fixed],
    /// Whether it takes its api's: not under a `#[header(none)]` of its group or its own.
    pub inherits: bool,
    /// The names of the api's headers it leaves, `#[header("name" = none)]` of its group or its own.
    pub leaves: &'static [&'static str],
}

impl RouteHeaders {
    /// The headers the route sends of `api`'s and its own, in the order a request writes them, each name once: its own
    /// first, then the api's it takes.
    pub fn sent<'a>(&'a self, api: &'a [Fixed]) -> impl Iterator<Item = &'a Fixed> {
        let api = api.iter().filter(move |header| {
            self.inherits
                && !self
                    .leaves
                    .iter()
                    .any(|left| left.eq_ignore_ascii_case(header.name))
                && !self
                    .own
                    .iter()
                    .any(|own| own.name.eq_ignore_ascii_case(header.name))
        });

        self.own.iter().chain(api)
    }
}

/// Whether `text` can be a fixed header's value as it is: visible ASCII, with spaces and tabs inside it only, which
/// HTTP would drop at either end. What `HeaderValue::from_static` takes, but the spaces at the ends.
pub const fn is_value(text: &str) -> bool {
    let bytes = text.as_bytes();
    let mut i = 0;

    while i < bytes.len() {
        let byte = bytes[i];
        let blank = byte == b' ' || byte == b'\t';

        if blank && (i == 0 || i == bytes.len() - 1) {
            return false;
        }
        if !blank && !(byte > b' ' && byte < 0x7f) {
            return false;
        }
        i += 1;
    }

    true
}

/// Whether the content type `value` keeps the media type `media_type` of a body quoll writes in UTF-8, whatever its
/// other parameters: what a fixed `Content-Type` may say of the body's, `application/json; charset=utf-8` of
/// `application/json`, and not `charset=iso-8859-1`. Media types and charsets are compared ignoring case.
pub const fn keeps_media_type(value: &str, media_type: &str) -> bool {
    let (essence, mut rest) = split_param(value.as_bytes());

    if !same_ascii(trim(essence), media_type.as_bytes()) {
        return false;
    }

    while let Some(params) = rest {
        let (param, after) = split_param(params);
        let (name, value) = split(param, b'=');
        let (name, value) = (
            trim(name),
            trim(match value {
                Some(value) => value,
                None => &[],
            }),
        );

        // A parameter is `name=value`, neither of them empty.
        if name.is_empty() || value.is_empty() {
            return false;
        }

        let Some(value) = unquoted(value) else {
            return false;
        };

        if same_ascii(name, b"charset") && !same_ascii(value, b"utf-8") {
            return false;
        }
        rest = after;
    }

    true
}

/// The text of a parameter's value: a token, which holds no quote, or what a quoted string holds, whose last quote
/// closes it and whose quotes inside are escaped, `"a \" b"`. `None` for any other, a quoted string left open, or
/// closed by an escaped quote, `"\"`, or a token with a quote in it.
pub(crate) const fn unquoted(value: &[u8]) -> Option<&[u8]> {
    let [b'"', inner @ ..] = value else {
        let mut i = 0;
        while i < value.len() {
            if value[i] == b'"' {
                return None;
            }
            i += 1;
        }

        return Some(value);
    };

    let mut i = 0;
    while i < inner.len() {
        match inner[i] {
            // A quoted pair, `\"`, is one character of the string.
            b'\\' => i += 1,
            b'"' if i == inner.len() - 1 => return Some(inner.split_at(i).0),
            b'"' => return None,
            _ => {}
        }
        i += 1;
    }

    None
}

/// `bytes` up to the first `;` outside a quoted string, as in `profile="a;b"`, and what follows it, `None` when there
/// is none.
pub(crate) const fn split_param(bytes: &[u8]) -> (&[u8], Option<&[u8]>) {
    let mut quoted = false;
    let mut i = 0;

    while i < bytes.len() {
        match bytes[i] {
            // A quoted pair, `\"`, is one character of the string.
            b'\\' if quoted => i += 1,
            b'"' => quoted = !quoted,
            b';' if !quoted => break,
            _ => {}
        }
        i += 1;
    }

    if i >= bytes.len() {
        return (bytes, None);
    }

    let (before, after) = bytes.split_at(i);
    let (_, rest) = after.split_at(1);

    (before, Some(rest))
}

/// `bytes` up to the first `separator`, and what follows it, `None` when there is no separator.
const fn split(bytes: &[u8], separator: u8) -> (&[u8], Option<&[u8]>) {
    let mut i = 0;
    while i < bytes.len() && bytes[i] != separator {
        i += 1;
    }

    let (before, after) = bytes.split_at(i);
    match after {
        [_, rest @ ..] => (before, Some(rest)),
        [] => (before, None),
    }
}

/// `bytes` without the spaces and tabs at either end.
const fn trim(mut bytes: &[u8]) -> &[u8] {
    while let [b' ' | b'\t', rest @ ..] = bytes {
        bytes = rest;
    }
    while let [rest @ .., b' ' | b'\t'] = bytes {
        bytes = rest;
    }

    bytes
}

/// Whether two ASCII texts are one, ignoring case.
const fn same_ascii(first: &[u8], second: &[u8]) -> bool {
    if first.len() != second.len() {
        return false;
    }

    let mut i = 0;
    while i < first.len() {
        if !first[i].eq_ignore_ascii_case(&second[i]) {
            return false;
        }
        i += 1;
    }

    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_value_is_visible_ascii_with_blanks_inside() {
        assert!(is_value("application/vnd.github+json"));
        assert!(is_value("files-api-2025-04-14, other"));
        assert!(is_value(""));
        assert!(!is_value(" 2023-06-01"));
        assert!(!is_value("2023-06-01\t"));
        assert!(!is_value("line\nbreak"));
        assert!(!is_value("caf\u{e9}"));
    }

    #[test]
    fn a_content_type_keeps_its_media_type_with_parameters() {
        assert!(keeps_media_type("application/json", "application/json"));
        assert!(keeps_media_type(
            "Application/JSON ; charset=UTF-8",
            "application/json"
        ));
        assert!(keeps_media_type(
            "text/plain; format=flowed; charset=\"utf-8\"",
            "text/plain"
        ));
        assert!(!keeps_media_type("text/plain", "application/json"));
        assert!(!keeps_media_type(
            "application/json-patch+json",
            "application/json"
        ));
        assert!(!keeps_media_type(
            "text/plain; charset=iso-8859-1",
            "text/plain"
        ));
        assert!(!keeps_media_type("application/json;", "application/json"));
        assert!(!keeps_media_type(
            "application/json; charset",
            "application/json"
        ));
        assert!(!keeps_media_type(
            "application/json; =x",
            "application/json"
        ));
        assert!(keeps_media_type(
            "application/json; profile=\"https://example.com/schema;v1\"; charset=utf-8",
            "application/json"
        ));
        assert!(keeps_media_type(
            "application/json; note=\"a \\\" ; b\"",
            "application/json"
        ));
        assert!(!keeps_media_type(
            "application/json; profile=\"open",
            "application/json"
        ));
        assert!(!keeps_media_type(
            r#"application/json; x="\""#,
            "application/json"
        ));
        assert!(!keeps_media_type(
            r#"application/json; x=a"b"#,
            "application/json"
        ));
        assert!(!keeps_media_type(
            r#"application/json; x="a"b""#,
            "application/json"
        ));
    }

    #[test]
    fn a_value_is_a_token_or_a_closed_quoted_string() {
        assert_eq!(unquoted(b"utf-8"), Some(&b"utf-8"[..]));
        assert_eq!(unquoted(br#""utf-8""#), Some(&b"utf-8"[..]));
        assert_eq!(unquoted(br#""a \" ; b""#), Some(&br#"a \" ; b"#[..]));
        assert_eq!(unquoted(br#""""#), Some(&b""[..]));
        assert_eq!(unquoted(br#""open"#), None);
        assert_eq!(unquoted(br#""\""#), None, "closed by an escaped quote");
        assert_eq!(unquoted(br#""a"b""#), None, "a quote inside");
        assert_eq!(unquoted(br#"a"b"#), None, "a quote in a token");
    }

    #[test]
    fn a_route_sends_its_own_headers_then_the_apis_it_takes() {
        let api = [
            Fixed {
                name: "Accept",
                value: "application/vnd.github+json",
            },
            Fixed {
                name: "X-Version",
                value: "1",
            },
            Fixed {
                name: "X-Beta",
                value: "files",
            },
        ];
        let route = RouteHeaders {
            own: &[Fixed {
                name: "accept",
                value: "application/vnd.github.raw+json",
            }],
            inherits: true,
            leaves: &["x-beta"],
        };
        let sent: Vec<(&str, &str)> = route
            .sent(&api)
            .map(|header| (header.name, header.value))
            .collect();

        assert_eq!(
            sent,
            [
                ("accept", "application/vnd.github.raw+json"),
                ("X-Version", "1")
            ]
        );

        let alone = RouteHeaders {
            inherits: false,
            ..route
        };

        assert_eq!(alone.sent(&api).count(), 1);
    }
}
