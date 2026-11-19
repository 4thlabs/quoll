//! The bodies a route reads that are not JSON: a form, text and bytes.
//!
//! The contract names them by their type, `login: Form<Login>`, `note: Text` or `file: Bytes`: the server reads the
//! body into it, and the client sends it with its content type.

use std::fmt;
use std::ops::{Deref, DerefMut};

/// A body sent as a form, `application/x-www-form-urlencoded`, read and written through serde_html_form as a query is:
/// a field per key, an `Option` that may be missing, a `Vec` that repeats its key.
///
/// It holds the `T`, and derefs to it, so that a handler reads `login.user`:
///
/// ```
/// use quoll::rest::Form;
///
/// struct Login {
///     user: String,
/// }
///
/// let login = Form(Login { user: "ana".into() });
///
/// assert_eq!(login.user, "ana");
/// ```
///
/// The server answers 415 to a request with another content type, and 400 to a form that does not read as `T` or
/// whose values are not UTF-8 once decoded.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Form<T>(pub T);

impl<T> Deref for Form<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.0
    }
}

impl<T> DerefMut for Form<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.0
    }
}

/// A body sent as text, `text/plain` in UTF-8.
///
/// It holds the text as a `String`, and derefs to a `str`:
///
/// ```
/// use quoll::rest::Text;
///
/// let text = Text::from("milk, eggs");
///
/// assert!(text.starts_with("milk"));
/// assert_eq!(text.0, "milk, eggs");
/// ```
///
/// The server answers 415 to a request with another content type or another charset than UTF-8, and 400 to a body that
/// is not UTF-8.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Text(pub String);

impl Deref for Text {
    type Target = str;

    fn deref(&self) -> &str {
        &self.0
    }
}

impl From<String> for Text {
    fn from(text: String) -> Self {
        Self(text)
    }
}

impl From<&str> for Text {
    fn from(text: &str) -> Self {
        Self(text.to_owned())
    }
}

/// Writes the text as it is.
impl fmt::Display for Text {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// A body sent as bytes, `application/octet-stream`.
///
/// It holds the bytes as a `Vec<u8>`, and derefs to a `[u8]`:
///
/// ```
/// use quoll::rest::Bytes;
///
/// let bytes = Bytes::from(vec![0xCA, 0xFE]);
///
/// assert_eq!(bytes.len(), 2);
/// assert_eq!(bytes.0, [0xCA, 0xFE]);
/// ```
///
/// The server answers 415 to a request with another content type, so that a form posted from another site, which a
/// browser sends without asking, never reaches the route.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Bytes(pub Vec<u8>);

impl Deref for Bytes {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.0
    }
}

impl From<Vec<u8>> for Bytes {
    fn from(bytes: Vec<u8>) -> Self {
        Self(bytes)
    }
}

impl From<&[u8]> for Bytes {
    fn from(bytes: &[u8]) -> Self {
        Self(bytes.to_vec())
    }
}
