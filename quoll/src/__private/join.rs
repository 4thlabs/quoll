//! A list written as the one value of a query key, as `#[join(sep)]` on a route's argument asks, `labels=bug,docs`:
//! the separator, the text of each value the client writes, and the list the server reads back.

use serde::de::{self, Deserialize, DeserializeOwned, Deserializer};

use crate::__private::text::{self, TextError};

#[cfg(feature = "client")]
pub(crate) mod write;

/// The separator of a list joined into one query value, one of the three styles OpenAPI describes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Join {
    /// `#[join(",")]`: `labels=bug,docs`, `style: form, explode: false`.
    Comma,
    /// `#[join(" ")]`: `labels=bug%20docs`, `style: spaceDelimited`.
    Space,
    /// `#[join("|")]`: `labels=bug|docs`, `style: pipeDelimited`.
    Pipe,
}

impl Join {
    /// The separator, once the query is decoded.
    pub const fn separator(self) -> char {
        match self {
            Self::Comma => ',',
            Self::Space => ' ',
            Self::Pipe => '|',
        }
    }

    /// The `style` of the parameter in OpenAPI, always with `explode: false`.
    pub const fn style(self) -> &'static str {
        match self {
            Self::Comma => "form",
            Self::Space => "spaceDelimited",
            Self::Pipe => "pipeDelimited",
        }
    }
}

/// `#[join(",")]`'s list, read back from its one value: serde's `deserialize_with` of the argument's field in the struct
/// the server reads the query into.
///
/// # Errors
///
/// When the value is not a string, or one of its pieces is not a `T`.
pub fn comma<'de, T: DeserializeOwned, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<Vec<T>, D::Error> {
    deserialize(Join::Comma, deserializer)
}

/// `#[join(" ")]`'s list, read back as [`comma`] reads its own.
///
/// # Errors
///
/// When the value is not a string, or one of its pieces is not a `T`.
pub fn space<'de, T: DeserializeOwned, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<Vec<T>, D::Error> {
    deserialize(Join::Space, deserializer)
}

/// `#[join("|")]`'s list, read back as [`comma`] reads its own.
///
/// # Errors
///
/// When the value is not a string, or one of its pieces is not a `T`.
pub fn pipe<'de, T: DeserializeOwned, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<Vec<T>, D::Error> {
    deserialize(Join::Pipe, deserializer)
}

/// The value, already decoded, split at each separator, each piece read as a path parameter is. The query is decoded
/// before it is split, so that `bug%2Cdocs`, which a browser's `URLSearchParams` writes, is the two values too. An
/// empty value is an empty list, as OpenAPI writes one; a missing key is one too, the field's `default`.
fn deserialize<'de, T: DeserializeOwned, D: Deserializer<'de>>(
    join: Join,
    deserializer: D,
) -> Result<Vec<T>, D::Error> {
    let value = String::deserialize(deserializer)?;
    if value.is_empty() {
        return Ok(Vec::new());
    }

    value
        .split(join.separator())
        .map(|piece| {
            text::from_text(Some(piece)).map_err(|error: TextError| {
                de::Error::custom(format!("the value `{piece}` of a joined list: {error}"))
            })
        })
        .collect()
}
