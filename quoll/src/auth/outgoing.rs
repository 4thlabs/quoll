//! A request as a [`ClientScheme`](crate::ClientScheme) writes its credential on it: what it reads of the request,
//! and what it adds, each value marked sensitive.

use std::fmt;

use http::header::AUTHORIZATION;
use http::{HeaderMap, HeaderName, HeaderValue, Method};

use super::credentials::{is_token, is_token68};

/// A request of a contract, as a [`ClientScheme`](crate::ClientScheme) writes its credential on it.
///
/// It reads the request's method, path, query and body, as a signature needs them, and adds a header, a cookie or a
/// key of the query. A header it adds is marked sensitive, so that a log of the request does not show it, and each
/// value is checked as the server would read it: one it would not read is an [`Unsendable`], which fails the call.
///
/// The client makes one for each request it sends. A test makes one with [`new`](Outgoing::new), to check what a
/// scheme writes without a server:
///
/// ```
/// use http::Method;
/// use http::header::AUTHORIZATION;
/// use quoll::auth::Outgoing;
///
/// let mut request = Outgoing::new(&Method::GET, "/Items", None, None);
///
/// request.authorization_params("MediaBrowser", &[("Token", r#"a"b"#)])?;
///
/// let written = &request.added_headers()[AUTHORIZATION];
/// assert_eq!(written, r#"MediaBrowser Token="a\"b""#);
/// assert!(written.is_sensitive());
/// # Ok::<(), quoll::auth::Unsendable>(())
/// ```
///
/// Its `Debug` shows the method, the path and the names of what was written, never a value, the query or the body.
pub struct Outgoing<'a> {
    method: &'a Method,
    path: &'a str,
    query: Option<&'a str>,
    body: Option<&'a [u8]>,
    pub(crate) added: Added,
}

/// What a scheme adds to a request, which the client puts on it once the scheme has written.
#[derive(Default)]
pub(crate) struct Added {
    /// Headers, each in the place of any of its name.
    pub(crate) headers: HeaderMap,
    /// `name=value` pairs, after the request's cookies, in one `Cookie` header.
    pub(crate) cookies: Vec<String>,
    /// Keys of the query, each in the place of any value of it.
    pub(crate) query: Vec<(String, String)>,
}

/// The message of a credential whose auth-scheme cannot be sent.
const NOT_AN_AUTH_SCHEME: &str =
    "the credential cannot be sent in `Authorization`: its auth-scheme is not a `token`";

impl fmt::Debug for Outgoing<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let cookies: Vec<&str> = self
            .added
            .cookies
            .iter()
            .map(|pair| pair.split_once('=').map_or(pair.as_str(), |(name, _)| name))
            .collect();
        let keys: Vec<&str> = self
            .added
            .query
            .iter()
            .map(|(key, _)| key.as_str())
            .collect();

        f.debug_struct("Outgoing")
            .field("method", self.method)
            .field("path", &self.path)
            .field("headers", &self.added.headers.keys().collect::<Vec<_>>())
            .field("cookies", &cookies)
            .field("query", &keys)
            .finish_non_exhaustive()
    }
}

impl<'a> Outgoing<'a> {
    /// The request of `method` to `path`, percent-encoded, with its `query`, without its `?`, and its `body`, `None`
    /// for a body that streams: what the client gives a scheme, and what a test gives one.
    pub fn new(
        method: &'a Method,
        path: &'a str,
        query: Option<&'a str>,
        body: Option<&'a [u8]>,
    ) -> Self {
        Self {
            method,
            path,
            query,
            body,
            added: Added::default(),
        }
    }

    /// The request's method.
    pub fn method(&self) -> &Method {
        self.method
    }

    /// The request's path, percent-encoded, as it is sent.
    pub fn path(&self) -> &str {
        self.path
    }

    /// The request's query, percent-encoded, without its `?`.
    pub fn query(&self) -> Option<&str> {
        self.query
    }

    /// The request's body, `None` for a body that streams, which no route of a contract sends.
    pub fn body(&self) -> Option<&[u8]> {
        self.body
    }

    /// Sets the header `name` to `value`, marked sensitive.
    ///
    /// # Errors
    ///
    /// When `value` is empty, or holds a control character, or a space or a tab at either end, which HTTP would drop.
    pub fn header(&mut self, name: HeaderName, value: &str) -> Result<(), Unsendable> {
        let blank = |c: char| c == ' ' || c == '\t';
        let sendable = !value.is_empty()
            && !value.starts_with(blank)
            && !value.ends_with(blank)
            && !value.chars().any(|c| c.is_control() && c != '\t');
        let value = sendable
            .then(|| HeaderValue::try_from(value).ok())
            .flatten()
            .ok_or(Unsendable::new(
                "the credential cannot be sent in its header: it is empty, or holds a control character or a space at either end",
            ))?;

        self.set(name, value);
        Ok(())
    }

    /// Sets `Authorization: <scheme> <token68>`, marked sensitive.
    ///
    /// # Errors
    ///
    /// When `scheme` is not a `token`, or `token` not a `token68`, letters, digits, `-._~+/` and `=` at its end.
    pub fn authorization_token68(&mut self, scheme: &str, token: &str) -> Result<(), Unsendable> {
        if !is_token(scheme) {
            return Err(Unsendable::new(NOT_AN_AUTH_SCHEME));
        }

        if !is_token68(token) {
            return Err(Unsendable::new(
                "the credential cannot be sent in `Authorization`: it is not a `token68`, letters, digits, `-._~+/` and `=` at its end",
            ));
        }

        let value = HeaderValue::try_from(format!("{scheme} {token}"))
            .expect("a token and a token68 make a header value");

        self.set(AUTHORIZATION, value);
        Ok(())
    }

    /// Sets `Authorization: <scheme> name="value", ...`, marked sensitive: each value a quoted-string, its `"` and
    /// `\` escaped, under its name as given.
    ///
    /// # Errors
    ///
    /// When `scheme` or a name is not a `token`, `params` is empty, or a value is empty or holds a character that is
    /// not visible ASCII or a space.
    pub fn authorization_params(
        &mut self,
        scheme: &str,
        params: &[(&str, &str)],
    ) -> Result<(), Unsendable> {
        if !is_token(scheme) {
            return Err(Unsendable::new(NOT_AN_AUTH_SCHEME));
        }

        if params.is_empty() {
            return Err(Unsendable::new(
                "the credential cannot be sent in `Authorization`: it has no param",
            ));
        }

        if !params.iter().all(|(name, _)| is_token(name)) {
            return Err(Unsendable::new(
                "the credential cannot be sent in `Authorization`: the name of a param is not a `token`",
            ));
        }

        let sendable = params.iter().all(|(_, value)| {
            !value.is_empty()
                && value
                    .bytes()
                    .all(|byte| byte == b' ' || byte.is_ascii_graphic())
        });

        if !sendable {
            return Err(Unsendable::new(
                "the credential cannot be sent in `Authorization`: a value is empty or holds a character that is not visible ASCII",
            ));
        }

        let mut text = format!("{scheme} ");
        for (i, (name, value)) in params.iter().enumerate() {
            if i > 0 {
                text.push_str(", ");
            }
            text.push_str(name);
            text.push_str("=\"");
            for c in value.chars() {
                if c == '"' || c == '\\' {
                    text.push('\\');
                }
                text.push(c);
            }
            text.push('"');
        }

        let value = HeaderValue::try_from(text).expect("visible ASCII makes a header value");

        self.set(AUTHORIZATION, value);
        Ok(())
    }

    /// Adds the cookie `name=value`, after the request's cookies, in one `Cookie` header marked sensitive.
    ///
    /// # Errors
    ///
    /// When `name` is not a `token`, or `value` is empty, or holds a character other than an ASCII letter, digit or
    /// sign, or `"`, `,`, `;` or `\`.
    pub fn cookie(&mut self, name: &str, value: &str) -> Result<(), Unsendable> {
        if !is_token(name) {
            return Err(Unsendable::new(
                "the credential cannot be sent as a cookie: its name is not a `token`",
            ));
        }

        let sendable = !value.is_empty()
            && value
                .bytes()
                .all(|byte| byte.is_ascii_graphic() && !b"\",;\\".contains(&byte));

        if !sendable {
            return Err(Unsendable::new(
                "the credential cannot be sent as a cookie: it is empty, or holds a character a cookie's value cannot",
            ));
        }

        self.added.cookies.push(format!("{name}={value}"));
        Ok(())
    }

    /// Sets the key `key` of the query to `value`, which a log of the URL shows.
    ///
    /// # Errors
    ///
    /// When `key` or `value` is empty.
    pub fn query_pair(&mut self, key: &str, value: &str) -> Result<(), Unsendable> {
        if key.is_empty() {
            return Err(Unsendable::new(
                "the credential cannot be sent in the query: its key is empty",
            ));
        }

        if value.is_empty() {
            return Err(Unsendable::new(
                "the credential cannot be sent in the query: it is empty",
            ));
        }

        self.added.query.retain(|(added, _)| added != key);
        self.added.query.push((key.to_owned(), value.to_owned()));
        Ok(())
    }

    /// The headers written so far, each marked sensitive.
    pub fn added_headers(&self) -> &HeaderMap {
        &self.added.headers
    }

    /// The cookies written so far, each `name=value`, in the order they were written.
    pub fn added_cookies(&self) -> impl Iterator<Item = &str> {
        self.added.cookies.iter().map(String::as_str)
    }

    /// The keys of the query written so far, each with its value, in the order they were written.
    pub fn added_query(&self) -> impl Iterator<Item = (&str, &str)> {
        self.added
            .query
            .iter()
            .map(|(key, value)| (key.as_str(), value.as_str()))
    }

    fn set(&mut self, name: HeaderName, mut value: HeaderValue) {
        value.set_sensitive(true);
        self.added.headers.insert(name, value);
    }
}

/// Why a client does not send a credential: one the server would not read. It fails the call, which is not sent, and
/// its message never holds the credential, as it is a `&'static str`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Unsendable(&'static str);

impl Unsendable {
    /// A credential that cannot be sent, for the reason `message`.
    pub const fn new(message: &'static str) -> Self {
        Self(message)
    }
}

impl fmt::Display for Unsendable {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for Unsendable {}

#[cfg(test)]
mod tests {
    use super::*;

    fn outgoing() -> Outgoing<'static> {
        Outgoing::new(&Method::GET, "/Items", None, None)
    }

    #[test]
    fn a_param_is_quoted_with_its_quote_and_backslash_escaped() {
        let mut request = outgoing();

        request
            .authorization_params("MediaBrowser", &[("Client", "web"), ("Token", r#"a"b\c"#)])
            .expect("sendable");

        let written = &request.added_headers()[AUTHORIZATION];
        assert_eq!(written, r#"MediaBrowser Client="web", Token="a\"b\\c""#);
        assert!(written.is_sensitive());
    }

    #[test]
    fn a_value_the_server_would_not_read_is_unsendable() {
        let params = |value| outgoing().authorization_params("MediaBrowser", &[("Token", value)]);
        for value in ["", "é", "a\nb", "a\tb"] {
            assert!(params(value).is_err(), "{value:?}");
        }
        assert!(
            outgoing()
                .authorization_params("MediaBrowser", &[])
                .is_err()
        );
        assert!(
            outgoing()
                .authorization_params("Media Browser", &[("Token", "k")])
                .is_err()
        );
        assert!(
            outgoing()
                .authorization_params("MediaBrowser", &[("To ken", "k")])
                .is_err()
        );

        for token in ["", "a b", "a=b", "é"] {
            assert!(
                outgoing().authorization_token68("token", token).is_err(),
                "{token:?}"
            );
        }

        for value in ["", " a", "a ", "a\nb"] {
            assert!(
                outgoing().header(AUTHORIZATION, value).is_err(),
                "{value:?}"
            );
        }

        for value in ["", "a;b", "a\"b", "a b"] {
            assert!(outgoing().cookie("session", value).is_err(), "{value:?}");
        }
        assert!(outgoing().cookie("a session", "s-1").is_err());

        assert!(outgoing().query_pair("token", "").is_err());
        assert!(outgoing().query_pair("", "t-1").is_err());
    }

    #[test]
    fn what_is_written_is_kept_once_per_name_and_each_header_marked_sensitive() {
        let mut request = outgoing();

        request.header(AUTHORIZATION, "first").expect("sendable");
        request
            .authorization_token68("token", "second")
            .expect("sendable");
        request.cookie("session", "s-1").expect("sendable");
        request.query_pair("token", "t-1").expect("sendable");
        request.query_pair("token", "t-2").expect("sendable");

        let headers = request.added_headers();
        assert_eq!(headers.len(), 1);
        assert_eq!(headers[AUTHORIZATION], "token second");
        assert!(headers[AUTHORIZATION].is_sensitive());
        assert_eq!(request.added_cookies().collect::<Vec<_>>(), ["session=s-1"]);
        assert_eq!(
            request.added_query().collect::<Vec<_>>(),
            [("token", "t-2")]
        );
    }

    #[test]
    fn an_unsendable_says_which_part_cannot_be_sent() {
        let message = |result: Result<(), Unsendable>| result.expect_err("unsendable").to_string();
        let params = |scheme, params: &[(&str, &str)]| {
            message(outgoing().authorization_params(scheme, params))
        };

        assert!(
            params("Media Browser", &[("Token", "k")])
                .ends_with("its auth-scheme is not a `token`")
        );
        assert!(
            message(outgoing().authorization_token68("to ken", "k"))
                .ends_with("its auth-scheme is not a `token`")
        );
        assert!(params("MediaBrowser", &[]).ends_with("it has no param"));
        assert!(
            params("MediaBrowser", &[("To ken", "k")])
                .ends_with("the name of a param is not a `token`")
        );
        assert!(
            params("MediaBrowser", &[("Token", "")])
                .ends_with("a value is empty or holds a character that is not visible ASCII")
        );
        assert!(
            message(outgoing().authorization_token68("token", "a b"))
                .contains("it is not a `token68`")
        );
        assert!(
            message(outgoing().cookie("a session", "s-1")).ends_with("its name is not a `token`")
        );
        assert!(message(outgoing().query_pair("", "t-1")).ends_with("its key is empty"));
        assert!(message(outgoing().query_pair("token", "")).ends_with("it is empty"));
    }

    #[test]
    fn its_debug_shows_the_names_written_and_no_value() {
        let mut request = Outgoing::new(
            &Method::POST,
            "/Items",
            Some("page=2&secret-q=1"),
            Some(b"secret-body"),
        );

        request
            .header(AUTHORIZATION, "secret-header")
            .expect("sendable");
        request
            .cookie("session", "secret-cookie")
            .expect("sendable");
        request
            .query_pair("token", "secret-token")
            .expect("sendable");

        let shown = format!("{request:?}");
        assert!(!shown.contains("secret"), "{shown}");
        for name in ["POST", "/Items", "authorization", "session", "token"] {
            assert!(shown.contains(name), "{name}: {shown}");
        }
    }

    #[test]
    fn an_unsendable_shows_its_message_alone() {
        let error = outgoing()
            .header(AUTHORIZATION, "secret\n")
            .expect_err("unsendable");

        assert!(!format!("{error} {error:?}").contains("secret"));
    }
}
