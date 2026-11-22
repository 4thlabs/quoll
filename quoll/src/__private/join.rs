//! A list written as the one value of a query key, as `#[join(sep)]` asks, `labels=bug,docs`: the separator, and
//! what [`rest::join`](crate::rest::join)'s types do with it.

use schemars::{JsonSchema, Schema, SchemaGenerator, json_schema};
use serde::de::{self, Deserialize, DeserializeOwned, Deserializer};
use serde::ser::{self, Serialize, Serializer};

use crate::__private::text::{self, TextError};

/// The key of the schema of a joined list that holds its mark, `{ "separator": ",", "schema": ... }`: the separator
/// and the schema of the list, which the document puts in place of the string as it writes the parameter's style.
pub(crate) const MARK: &str = "x-quoll-join";

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

    /// The separator a schema's mark holds.
    pub(crate) fn from_mark(mark: &str) -> Option<Self> {
        [Self::Comma, Self::Space, Self::Pipe]
            .into_iter()
            .find(|join| mark.chars().eq([join.separator()]))
    }
}

/// Why a list cannot be joined into one value the server reads back as it is.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Refused {
    /// A value is not one piece of text, or is a `None`.
    NotText,
    /// A value holds the separator, which the server would read as two values.
    Separator(String),
    /// The list is one empty value, which the server would read as an empty list.
    OneEmpty,
}

impl Refused {
    /// What is refused, said of a value of `what`.
    pub(crate) fn message(&self, join: Join, what: &str) -> String {
        match self {
            Self::NotText => format!("a value of {what} is not written as one piece of text"),
            Self::Separator(value) => format!(
                "the value `{value}` of {what} holds the separator `{}`: the server would read it as two values",
                join.separator()
            ),
            Self::OneEmpty => {
                format!("{what} is one empty value, which the server would read as an empty list")
            }
        }
    }
}

/// The text of each value of `values`, each given to `push` with whether it is the first, so that the caller writes
/// the separator and escapes the value as its place needs; `Ok(false)` for an empty list, which nothing is written for.
pub(crate) fn each_text<T: Serialize>(
    join: Join,
    values: &[T],
    mut push: impl FnMut(bool, &str),
) -> Result<bool, Refused> {
    let mut empty = true;

    for (index, value) in values.iter().enumerate() {
        let Some(Some(text)) = text::to_text(value) else {
            return Err(Refused::NotText);
        };
        if text.contains(join.separator()) {
            return Err(Refused::Separator(text));
        }

        empty &= text.is_empty();
        push(index == 0, &text);
    }

    if values.len() == 1 && empty {
        return Err(Refused::OneEmpty);
    }

    Ok(!values.is_empty())
}

/// `values` written as one value, joined with the separator, or as nothing, which leaves the key out, for an empty
/// list.
pub(crate) fn serialize<T: Serialize, S: Serializer>(
    join: Join,
    values: &[T],
    serializer: S,
) -> Result<S::Ok, S::Error> {
    let mut joined = String::new();
    let written = each_text(join, values, |first, text| {
        if !first {
            joined.push(join.separator());
        }
        joined.push_str(text);
    })
    .map_err(|refused| ser::Error::custom(refused.message(join, "a joined list")))?;

    if written {
        serializer.serialize_str(&joined)
    } else {
        serializer.serialize_none()
    }
}

/// The value, already decoded, split at each separator, each piece read as a path parameter is. The query is decoded
/// before it is split, so that `bug%2Cdocs`, which a browser's `URLSearchParams` writes, is the two values too. An
/// empty value is an empty list, as OpenAPI writes one, and so is a `null`, which `serialize` writes for one in a
/// format other than a query, JSON.
pub(crate) fn deserialize<'de, T: DeserializeOwned, D: Deserializer<'de>>(
    join: Join,
    deserializer: D,
) -> Result<Vec<T>, D::Error> {
    let value = Option::<String>::deserialize(deserializer)?.unwrap_or_default();
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

/// The schema of what `serialize` writes, a string or `null` for an empty list, marked with the separator and the
/// schema of the list itself.
pub(crate) fn json_schema<T: JsonSchema>(join: Join, generator: &mut SchemaGenerator) -> Schema {
    let list = Vec::<T>::json_schema(generator);

    json_schema!({
        "type": ["string", "null"],
        MARK: { "separator": join.separator().to_string(), "schema": list },
    })
}
