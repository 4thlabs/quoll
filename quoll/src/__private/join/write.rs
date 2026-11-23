//! The text of each value of a joined list the client writes, and what it refuses: a list the server would not read
//! back as it is.

use serde::Serialize;

use super::Join;
use crate::__private::text;

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
        let Some(Some(text)) = text::write::to_text(value) else {
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
