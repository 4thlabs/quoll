//! An error as the RFC 9457 problem quoll's server answers for it: the `type` and the extension members a handler
//! gives it, and the problem written from them.

use std::fmt;

use http::header::CONTENT_TYPE;
use http::{HeaderMap, HeaderValue, StatusCode, Version};
use serde::Serialize;

use super::{BODY_LIMIT, Error, ErrorKind, Repr};
use crate::__private::content_type::PROBLEM;
use crate::Response;
use crate::problem::Answer;

/// The members RFC 9457 gives every problem, which no extension member is named as.
const RFC_MEMBERS: [&str; 5] = ["type", "title", "status", "detail", "instance"];

/// The `type` of a problem that says no more than its status.
const ABOUT_BLANK: &str = "about:blank";

impl Error {
    /// The error, its problem of the `type` `uri`, the URI naming the kind of problem, which a client tells two errors
    /// of one status apart by, in place of `about:blank`. A second `type` replaces the first.
    ///
    /// `uri` is a URI reference of RFC 3986: absolute, `https://example.com/problems/app-running`, or relative,
    /// `/problems/app-running`. quoll's client gives it as it is written, in [`Problem::kind`](crate::Problem::kind):
    /// resolving a relative one against the URL called, as the RFC has it, is the caller's.
    ///
    /// ```
    /// use http::StatusCode;
    /// use quoll::Error;
    ///
    /// let error = Error::new(StatusCode::CONFLICT, "alpha is running")
    ///     .with_type("/problems/app-running")
    ///     .with_member("app", "alpha");
    ///
    /// assert_eq!(error.status(), Some(StatusCode::CONFLICT));
    /// ```
    ///
    /// A `type` that is empty or not a URI reference, as a text with a space, is left out: the server answers the
    /// error all the same, with its status and its members, and the refusal is logged at `WARN` when the error is
    /// built, without the `type`. An error that sends its status alone, an internal one, one received from another
    /// API, or one of a status that is not an error's, `4xx` or `5xx`, which is answered 500, sends no `type` either,
    /// which is logged at `DEBUG`.
    #[must_use]
    pub fn with_type(mut self, uri: impl Into<String>) -> Self {
        let uri = uri.into();

        if let Err(refused) = checked_type(&uri) {
            log_refused(&refused);
            return self;
        }

        let answered = self.answer().status;

        let Some((kind, _)) = self.sent_mut() else {
            log_not_sent(answered, "its type");
            return self;
        };

        *kind = Some(uri);
        self
    }

    /// The error, its problem with the extension member `name`, whose value is the JSON of `value`: what a client
    /// reads with `body_as`, beside the problem's own members. A second member of one name replaces the first.
    /// [`with_type`](Error::with_type) shows a call.
    ///
    /// `name` is one RFC 9457 has a client read: a letter, then letters, digits or `_`, three characters at least, and
    /// none of the RFC's own members, `type`, `title`, `status`, `detail` or `instance`. A member of another name, or
    /// whose value does not serialize as JSON, as a map whose keys are not strings, is left out: the server answers
    /// the error all the same, with its status and its other members, and the refusal is logged at `WARN` when the
    /// error is built, with the member's name and never its value. An error that sends its status alone, an internal
    /// one, one received from another API, or one of a status that is not an error's, `4xx` or `5xx`, which is
    /// answered 500, sends no member either, which is logged at `DEBUG`.
    #[must_use]
    pub fn with_member(mut self, name: impl Into<String>, value: impl Serialize) -> Self {
        let name = name.into();

        if let Err(refused) = checked_name(&name) {
            log_refused(&refused);
            return self;
        }

        let Ok(value) = serde_json::to_value(value) else {
            log_refused(&Refused::Value(name));
            return self;
        };
        let answered = self.answer().status;

        let Some((_, members)) = self.sent_mut() else {
            log_not_sent(answered, &format!("its extension member `{name}`"));
            return self;
        };

        members.insert(name, value);
        self
    }

    /// The error as a client receives it from quoll's server: the answer of the status the server answers, whose
    /// [detail](Error::detail) is the problem the server writes, its `type` among them, whose [body](Error::body) is
    /// that problem's JSON, its extension members among them, and whose `Display` says its detail,
    /// `another api answered 409: alpha is running`. Its [response](Error::response) has the content type
    /// `application/problem+json`, then the error's [headers](Error::headers) when the server sends them, with the
    /// error's own status; its `Retry-After` among them.
    ///
    /// A fake of an api of problems returns it, so that the code it serves reads what the real client gives. An
    /// internal error is a 500 that says nothing more, and one received from another API a 502. A problem longer than
    /// the 64 KiB the client keeps of a body is cut there, and one whose JSON nests deeper than the client reads it,
    /// 128 levels, is read as no problem, as the client reads them: it has no detail and no response, and says its
    /// status alone.
    ///
    /// ```
    /// use http::StatusCode;
    /// use quoll::Error;
    ///
    /// let error = Error::new(StatusCode::CONFLICT, "alpha is running")
    ///     .with_type("/problems/app-running")
    ///     .into_answered();
    ///
    /// assert_eq!(error.detail().map(quoll::Problem::kind), Some("/problems/app-running"));
    /// assert_eq!(error.to_string(), "another api answered 409: alpha is running");
    /// ```
    pub fn into_answered(self) -> Self {
        let answer = self.answer();
        let status = answer.status;
        let mut body = answer.to_json();
        let problem = answer.to_problem();

        let mut headers = HeaderMap::from_iter([(CONTENT_TYPE, HeaderValue::from_static(PROBLEM))]);

        // The server sends the headers with the error's own status alone, as it sends its detail.
        if answer.detail.is_some() {
            for (name, value) in self.headers() {
                headers.append(name, value.clone());
            }
        }

        // The client keeps 64 KiB of a body, and reads a problem as JSON, a JSON nested deeper than its limit
        // reading as none: a problem it does not read has no detail.
        let read =
            body.len() <= BODY_LIMIT && serde_json::from_slice::<serde_json::Value>(&body).is_ok();

        if !read {
            body.truncate(BODY_LIMIT);

            return Self::upstream(status, None, Some(body), None).received(headers);
        }

        let message = problem
            .detail()
            .filter(|detail| !detail.is_empty())
            .map(str::to_owned);
        let answer = Response::from_parts(status, Version::HTTP_11, headers, problem);

        Self::upstream(status, Some(answer), Some(body), message)
    }

    /// The `type` and the members of the error, when the server sends them: an error of a status that is an error's.
    fn sent_mut(
        &mut self,
    ) -> Option<(
        &mut Option<String>,
        &mut serde_json::Map<String, serde_json::Value>,
    )> {
        match &mut self.0.repr {
            Repr::Problem {
                status,
                kind,
                members,
                ..
            } if status.is_client_error() || status.is_server_error() => Some((kind, members)),
            _ => None,
        }
    }
}

impl<E> Error<E> {
    /// The problem quoll's server answers for this error: its status, and the `type`, the detail and the members a
    /// handler gave when they are sent, an error's status, `4xx` or `5xx`; 500 for an internal error and for a
    /// status that is not an error's, 502 for what another API answered, and 500 or 502 for a call that got no
    /// answer. Nothing is logged.
    pub(crate) fn answer(&self) -> Answer<'_> {
        let (status, sent) = match &self.0.repr {
            Repr::Problem {
                status,
                detail,
                kind,
                members,
            } if status.is_client_error() || status.is_server_error() => {
                (*status, Some((detail, kind, members)))
            }
            Repr::Problem { .. } | Repr::Internal(_) => (StatusCode::INTERNAL_SERVER_ERROR, None),
            Repr::Upstream { .. } => (StatusCode::BAD_GATEWAY, None),
            Repr::Failed { kind, .. } => (failed_status(*kind), None),
        };

        let Some((detail, kind, members)) = sent else {
            return Answer {
                kind: ABOUT_BLANK,
                title: status.canonical_reason(),
                status,
                detail: None,
                members: None,
            };
        };

        Answer {
            kind: kind.as_deref().unwrap_or(ABOUT_BLANK),
            title: status.canonical_reason(),
            status,
            detail: Some(detail),
            members: Some(members),
        }
    }
}

/// What a handler answers for a call of its client that got no answer to read: 500 for the handler's own mistake, a
/// URL that is not one, and 502 for the other API's.
pub(crate) fn failed_status(kind: ErrorKind) -> StatusCode {
    match kind {
        ErrorKind::Invalid => StatusCode::INTERNAL_SERVER_ERROR,
        _ => StatusCode::BAD_GATEWAY,
    }
}

/// A `type` or an extension member that a problem leaves out, and why, without its value.
#[derive(Debug, PartialEq, Eq)]
enum Refused {
    /// An empty `type`.
    EmptyType,
    /// A `type` that is not a URI reference.
    Type,
    /// An extension member named as one of the RFC's own.
    RfcMember(String),
    /// An extension member whose name is not one of RFC 9457's.
    Name(String),
    /// A member whose value does not serialize as JSON.
    Value(String),
}

impl fmt::Display for Refused {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Refused::EmptyType => f.write_str("a problem's type is empty"),
            Refused::Type => f.write_str(
                "a problem's type is not a URI: write an absolute one, `https://example.com/problems/app-running`, or a relative one, `/problems/app-running`",
            ),
            Refused::RfcMember(name) => write!(
                f,
                "`{name}` is a member of every problem, which an extension member is not named as: give it another name"
            ),
            Refused::Name(name) => write!(
                f,
                "`{name}` is not the name of an extension member: write a letter, then letters, digits or `_`, three characters at least, as RFC 9457 has it"
            ),
            Refused::Value(name) => {
                write!(f, "the extension member `{name}` does not serialize as JSON")
            }
        }
    }
}

/// Logs at `WARN` a `type` or a member left out, and why, never its value. With neither feature, which bring
/// `tracing`, no server answers the error and no fake stands for a client, and nothing is logged.
fn log_refused(refused: &Refused) {
    #[cfg(any(feature = "server", feature = "client"))]
    tracing::warn!("{refused}: a handler's error is answered without it");
    #[cfg(not(any(feature = "server", feature = "client")))]
    let _ = refused;
}

/// Logs at `DEBUG` what an error answered with `status` alone does not send, as [`log_refused`] logs.
fn log_not_sent(status: StatusCode, what: &str) {
    #[cfg(any(feature = "server", feature = "client"))]
    tracing::debug!("a handler's error answered {status} alone does not send {what}");
    #[cfg(not(any(feature = "server", feature = "client")))]
    let _ = (status, what);
}

/// Whether `uri` is a `type`: a URI reference.
fn checked_type(uri: &str) -> Result<(), Refused> {
    if uri.is_empty() {
        return Err(Refused::EmptyType);
    }

    if !is_uri_reference(uri) {
        return Err(Refused::Type);
    }

    Ok(())
}

/// Whether `name` is the name of an extension member: RFC 9457's, `ALPHA *(ALPHA / DIGIT / "_")` of three characters
/// at least, and none of the RFC's own members.
fn checked_name(name: &str) -> Result<(), Refused> {
    if RFC_MEMBERS.contains(&name) {
        return Err(Refused::RfcMember(name.to_owned()));
    }

    let named = name.len() >= 3
        && name.starts_with(|first: char| first.is_ascii_alphabetic())
        && name
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_');

    if !named {
        return Err(Refused::Name(name.to_owned()));
    }

    Ok(())
}

/// Whether `text`, not empty, is a URI reference of RFC 3986, absolute or relative: every byte one a URI holds, a `%`
/// the start of an escape, a `:` before the first `/`, `?` or `#` the end of a scheme, a `[` or a `]` in the host of
/// an authority alone, after `//`, and one `#` at most.
fn is_uri_reference(text: &str) -> bool {
    let bytes = text.as_bytes();
    let mut at = 0;

    while let Some(&byte) = bytes.get(at) {
        if byte == b'%' {
            let escaped = bytes.get(at + 1..at + 3);

            if !escaped.is_some_and(|hex| hex.iter().all(u8::is_ascii_hexdigit)) {
                return false;
            }

            at += 3;
            continue;
        }

        if !(byte.is_ascii_alphanumeric() || b"-._~:/?#[]@!$&'()*+,;=".contains(&byte)) {
            return false;
        }

        at += 1;
    }

    if text.bytes().filter(|&byte| byte == b'#').count() > 1 {
        return false;
    }

    // A relative reference has no `:` in its first segment, which would read as a scheme.
    let head = text.split(['/', '?', '#']).next().unwrap_or_default();
    let after_scheme = match head.split_once(':') {
        None => 0,
        Some((scheme, _)) if is_scheme(scheme) => scheme.len() + 1,
        Some(_) => return false,
    };

    // The authority, after `//`, runs to the first `/`, `?` or `#`: the one place of brackets, an IP literal's.
    let authority = text[after_scheme..].strip_prefix("//").map(|rest| {
        let start = after_scheme + 2;

        start..start + rest.find(['/', '?', '#']).unwrap_or(rest.len())
    });

    if authority
        .as_ref()
        .is_some_and(|authority| !is_authority(&text[authority.clone()]))
    {
        return false;
    }

    text.bytes()
        .enumerate()
        .filter(|&(_, byte)| byte == b'[' || byte == b']')
        .all(|(at, _)| authority.as_ref().is_some_and(|host| host.contains(&at)))
}

/// Whether `authority`, whose bytes are those of a URI, is one: a userinfo and an `@`, which it may leave out, a host,
/// an IP literal in brackets or a name with neither brackets nor a `:`, then a `:` and a port of digits, which it may
/// leave out too.
fn is_authority(authority: &str) -> bool {
    let (userinfo, host_port) = match authority.rsplit_once('@') {
        Some((userinfo, host_port)) => (userinfo, host_port),
        None => ("", authority),
    };

    if userinfo.contains(['@', '[', ']']) {
        return false;
    }

    let (host, port) = match host_port.strip_prefix('[') {
        Some(literal) => {
            let Some((inside, port)) = literal.split_once(']') else {
                return false;
            };

            if inside.is_empty() || inside.contains('[') {
                return false;
            }

            (inside, port)
        }
        None => match host_port.find(':') {
            Some(colon) => (&host_port[..colon], &host_port[colon..]),
            None => (host_port, ""),
        },
    };

    if host.contains(['[', ']']) || !host_port.starts_with('[') && host.contains(':') {
        return false;
    }

    match port.strip_prefix(':') {
        Some(digits) => digits.bytes().all(|byte| byte.is_ascii_digit()),
        None => port.is_empty(),
    }
}

/// Whether `scheme` is the scheme of a URI: a letter, then letters, digits, `+`, `-` or `.`.
fn is_scheme(scheme: &str) -> bool {
    scheme.starts_with(|first: char| first.is_ascii_alphabetic())
        && scheme
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || b"+-.".contains(&byte))
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use serde_json::{Value, json};

    use super::*;

    fn conflict() -> Error {
        Error::new(StatusCode::CONFLICT, "alpha is running")
    }

    fn written(error: &Error) -> Value {
        serde_json::from_slice(&error.answer().to_json()).expect("a problem is JSON")
    }

    #[test]
    fn a_type_and_members_are_written_after_the_rfcs_own_members() {
        let error = conflict()
            .with_type("https://example.com/problems/app-running")
            .with_member("app", "alpha")
            .with_member("ports", [80, 443]);

        assert_eq!(
            written(&error),
            json!({
                "type": "https://example.com/problems/app-running",
                "title": "Conflict",
                "status": 409,
                "detail": "alpha is running",
                "app": "alpha",
                "ports": [80, 443],
            })
        );
        assert_eq!(
            String::from_utf8(error.answer().to_json()).expect("JSON is UTF-8"),
            r#"{"type":"https://example.com/problems/app-running","title":"Conflict","status":409,"detail":"alpha is running","app":"alpha","ports":[80,443]}"#,
            "the RFC's own members come first"
        );
    }

    #[test]
    fn a_second_type_or_member_of_one_name_replaces_the_first() {
        let error = conflict()
            .with_type("/problems/a")
            .with_type("/problems/b")
            .with_member("app", "alpha")
            .with_member("app", "beta");
        let problem = written(&error);

        assert_eq!(problem["type"], "/problems/b");
        assert_eq!(problem["app"], "beta");
    }

    #[test]
    fn a_refused_type_or_member_is_left_out_and_the_rest_is_sent() {
        let map = BTreeMap::from([((1, 2), "a key that is no string")]);
        let error = conflict()
            .with_type("/problems/app-running")
            .with_type("app running")
            .with_member("status", 200)
            .with_member("x", 1)
            .with_member("map", map)
            .with_member("app", "alpha");

        assert_eq!(
            written(&error),
            json!({
                "type": "/problems/app-running",
                "title": "Conflict",
                "status": 409,
                "detail": "alpha is running",
                "app": "alpha",
            })
        );
    }

    #[test]
    fn a_member_is_named_as_rfc_9457_has_it() {
        for name in RFC_MEMBERS {
            assert_eq!(checked_name(name), Err(Refused::RfcMember(name.to_owned())));
        }

        for name in ["app", "Status", "app_id", "a42"] {
            assert_eq!(checked_name(name), Ok(()), "{name}");
        }

        for name in ["", "ab", "_app", "4app", "app-id", "app id", "appé"] {
            assert_eq!(
                checked_name(name),
                Err(Refused::Name(name.to_owned())),
                "{name}"
            );
        }
    }

    #[test]
    fn each_refusal_says_why_without_the_value() {
        assert_eq!(Refused::EmptyType.to_string(), "a problem's type is empty");
        assert_eq!(
            Refused::Type.to_string(),
            "a problem's type is not a URI: write an absolute one, `https://example.com/problems/app-running`, or a relative one, `/problems/app-running`"
        );
        assert_eq!(
            Refused::RfcMember("status".into()).to_string(),
            "`status` is a member of every problem, which an extension member is not named as: give it another name"
        );
        assert_eq!(
            Refused::Name("app-id".into()).to_string(),
            "`app-id` is not the name of an extension member: write a letter, then letters, digits or `_`, three characters at least, as RFC 9457 has it"
        );
        assert_eq!(
            Refused::Value("map".into()).to_string(),
            "the extension member `map` does not serialize as JSON"
        );
    }

    #[test]
    fn a_type_is_a_uri_reference() {
        let valid = [
            "about:blank",
            "https://example.com/problems/out-of-credit",
            "urn:problem:app-running",
            "/problems/app-running",
            "app-running",
            "../problems/a%20b?x=1#y",
            "tag:example.com,2026:app",
            "http://[::1]:8080/problems/a",
            "//[::1]/problems/a",
            "https://user:secret@example.com:8443/problems/a",
            "https://example.com:/problems/a",
            "file:///problems/a",
        ];
        let invalid = [
            "app running",
            "/é",
            "%2",
            "%zz",
            "1http://x",
            ":x",
            "a b:c",
            "<x>",
            "/problems/a[b]",
            "http://x/[a]",
            "[::1]/a",
            "a#b#c",
            "https://[broken/problem",
            "https://user[x]@example.com/problem",
            "https://example.com:abc/problem",
            "https://a@b@example.com/problem",
            "https://[]/problem",
            "https://[::1]x/problem",
        ];

        for uri in valid {
            assert_eq!(checked_type(uri), Ok(()), "{uri}");
        }

        for uri in invalid {
            assert_eq!(checked_type(uri), Err(Refused::Type), "{uri}");
        }

        assert_eq!(checked_type(""), Err(Refused::EmptyType));
    }

    #[test]
    fn an_error_whose_problem_is_quolls_own_leaves_its_type_and_members() {
        let internal = Error::internal("down")
            .with_type("/problems/down")
            .with_member("app", "alpha");
        let not_an_error = Error::new(StatusCode::OK, "fine").with_type("/problems/fine");

        for error in [internal, not_an_error] {
            assert_eq!(
                written(&error),
                json!({ "type": "about:blank", "title": "Internal Server Error", "status": 500 })
            );
        }
    }

    #[test]
    fn an_answered_error_is_what_the_client_reads() {
        let error = conflict()
            .with_type("/problems/app-running")
            .with_member("app", "alpha")
            .into_answered();
        let problem = error.detail().expect("a problem");
        let response = error.response().expect("an answer");
        let body: Value = serde_json::from_slice(error.body().expect("a body")).expect("JSON");

        assert_eq!(error.status(), Some(StatusCode::CONFLICT));
        assert_eq!(problem.kind(), "/problems/app-running");
        assert_eq!(problem.title(), Some("Conflict"));
        assert_eq!(problem.status(), Some(StatusCode::CONFLICT));
        assert_eq!(problem.detail(), Some("alpha is running"));
        assert_eq!(body["app"], "alpha");
        assert_eq!(
            error.to_string(),
            "another api answered 409: alpha is running"
        );
        assert_eq!(response.headers().len(), 1);
        assert_eq!(response.headers()[CONTENT_TYPE], PROBLEM);

        let limited = Error::new(StatusCode::TOO_MANY_REQUESTS, "too many sign-ins")
            .retry_after(std::time::Duration::from_secs(30))
            .into_answered();

        assert_eq!(limited.headers()[http::header::RETRY_AFTER], "30");
        assert_eq!(limited.headers()[CONTENT_TYPE], PROBLEM);

        let internal = Error::internal("down")
            .with_header("x-note", "kept out")
            .into_answered();

        assert!(internal.headers().get("x-note").is_none(), "a 500 alone");

        let internal = Error::internal("down").into_answered();

        assert_eq!(internal.status(), Some(StatusCode::INTERNAL_SERVER_ERROR));
        assert_eq!(
            internal.detail().map(crate::Problem::kind),
            Some("about:blank")
        );
        assert_eq!(internal.to_string(), "another api answered 500");
    }

    #[test]
    fn the_debug_form_names_the_members_and_not_their_values() {
        let error = conflict()
            .with_type("/problems/app-running")
            .with_member("app", "a secret value");
        let debug = format!("{error:?}");

        assert!(debug.contains("/problems/app-running"), "{debug}");
        assert!(debug.contains(r#"members: ["app"]"#), "{debug}");
        assert!(!debug.contains("secret"), "{debug}");
    }

    #[test]
    fn an_answered_problem_nested_past_the_clients_limit_has_no_detail() {
        let mut nested = json!(1);

        for _ in 0..200 {
            nested = json!([nested]);
        }

        let error = conflict().with_member("nested", nested).into_answered();

        assert_eq!(error.status(), Some(StatusCode::CONFLICT));
        assert!(error.body().is_some_and(|body| body.len() < BODY_LIMIT));
        assert!(error.detail().is_none() && error.response().is_none());
        assert_eq!(error.to_string(), "another api answered 409");
    }

    #[test]
    fn an_answered_problem_past_the_clients_limit_is_cut_with_no_detail() {
        let long = "a".repeat(BODY_LIMIT);
        let error = conflict().with_member("long", long).into_answered();

        assert_eq!(error.status(), Some(StatusCode::CONFLICT));
        assert_eq!(error.body().map(<[u8]>::len), Some(BODY_LIMIT));
        assert_eq!(error.headers()[CONTENT_TYPE], PROBLEM);
        assert!(error.detail().is_none() && error.response().is_none());
        assert_eq!(error.to_string(), "another api answered 409");
    }
}
