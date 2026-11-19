//! The bodies a route reads that are not JSON, a form, text and bytes; text and bytes are results too.
//!
//! The contract names them by their type, `login: Form<Login>`, `note: Text` or `file: Bytes`: the server reads the
//! body into it, and the client sends it with its content type. As a result, `-> Text` or `-> Bytes`, the server
//! answers it with its content type, and the client reads it back.

use std::borrow::Cow;
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

/// A body or a result sent as text, `text/plain` in UTF-8.
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
/// is not UTF-8. As a result, it is answered `text/plain; charset=utf-8`, and the client refuses an answer of another
/// content type or charset, or one that is not UTF-8, with an error that has no status.
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

/// A body or a result sent as bytes, of the content type they carry: `application/octet-stream` unless they say
/// another.
///
/// It holds the bytes as a `Vec<u8>`, and derefs to a `[u8]`; [`Bytes::content_type`] says what they are:
///
/// ```
/// use quoll::rest::Bytes;
///
/// let png = Bytes::new(vec![0x89, b'P', b'N', b'G']).with_content_type("image/png");
///
/// assert_eq!(png.content_type(), "image/png");
/// assert_eq!(png.len(), 4);
/// assert_eq!(png.0, [0x89, b'P', b'N', b'G']);
///
/// assert_eq!(Bytes::from(vec![0xCA, 0xFE]).content_type(), "application/octet-stream");
/// ```
///
/// As a result, the server answers them with their content type. The client reads an answer of any content type into
/// them, whole, as it does a JSON result, and keeps that type: an answer without one, or with one that is not a media
/// type, is `application/octet-stream`, bytes of no known type, as RFC 9110 lets a recipient assume.
///
/// As a body, the client sends them with their content type. The server reads a body of any media type into them, but
/// the three a browser sends to another site without asking it first, `application/x-www-form-urlencoded`,
/// `multipart/form-data` and `text/plain`, and a body without one: those answer 415, so that what a page of another
/// site posts never reaches the route.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Bytes(pub Vec<u8>, pub(crate) Cow<'static, str>);

impl Bytes {
    /// `bytes`, of no known type: `application/octet-stream`.
    pub fn new(bytes: impl Into<Vec<u8>>) -> Self {
        Self(bytes.into(), Cow::Borrowed(OCTET_STREAM))
    }

    /// The same bytes, of `content_type`, a media type as `Content-Type` writes it: `image/png`, or
    /// `text/csv; charset=utf-8`.
    ///
    /// It is checked when it is sent: a server answers a content type that is not a media type with a 500, logged,
    /// and a client refuses it with a misuse error, before sending.
    #[must_use]
    pub fn with_content_type(self, content_type: impl Into<String>) -> Self {
        Self(self.0, Cow::Owned(content_type.into()))
    }

    /// What the bytes are, as `Content-Type` says it, as it was given or received: `application/octet-stream` when
    /// nothing said it.
    pub fn content_type(&self) -> &str {
        &self.1
    }
}

/// What a `Bytes` is when nothing says otherwise.
pub(crate) const OCTET_STREAM: &str = "application/octet-stream";

/// No bytes, `application/octet-stream`.
impl Default for Bytes {
    fn default() -> Self {
        Self::new(Vec::new())
    }
}

impl Deref for Bytes {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.0
    }
}

impl AsRef<[u8]> for Bytes {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

/// The bytes, `application/octet-stream`.
impl From<Vec<u8>> for Bytes {
    fn from(bytes: Vec<u8>) -> Self {
        Self::new(bytes)
    }
}

/// The bytes, copied, `application/octet-stream`.
impl From<&[u8]> for Bytes {
    fn from(bytes: &[u8]) -> Self {
        Self::new(bytes)
    }
}
