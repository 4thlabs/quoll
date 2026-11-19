//! The types a contract names that the macro recognizes by their name: `Created` as a result, `Form`, `Text` and
//! `Bytes` as a body.
//!
//! The macro writes them with the contract's own tokens, so that the user's import is the one that names them. Each
//! trait here is implemented by the crate's type alone: a type of another crate, or of the user's, that has one of
//! these names fails on it, with a message that says so. The checks of [`created`], [`form`], [`text`] and [`bytes`]
//! are written whatever the features of `quoll`, so that a contract crate built with neither feature checks them too.

use crate::rest::{Bytes, Created, Form, Text};

/// The crate's [`Created`]: its location and its value, which the server answers.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Created`, which a result named `Created` is read as",
    label = "not `quoll::rest::Created`",
    note = "bring quoll's into scope, `use quoll::rest::Created;`, or name a type of your own otherwise"
)]
pub trait IsCreated: Sized {
    /// The `T` of `Created<T>`, the body.
    type Value;

    /// The location, sent as `Location` when there is one, and the value.
    fn into_parts(self) -> (Option<String>, Self::Value);
}

impl<T> IsCreated for Created<T> {
    type Value = T;

    fn into_parts(self) -> (Option<String>, T) {
        (self.location, self.value)
    }
}

/// The crate's [`Form`], whose `T` is read and written as a form.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Form`, which a body named `Form` is read as",
    label = "not `quoll::rest::Form`",
    note = "bring quoll's into scope, `use quoll::rest::Form;`, or name a type of your own otherwise"
)]
pub trait IsForm: Sized {
    /// The `T` of `Form<T>`.
    type Value;

    /// The form holding `value`.
    fn from_value(value: Self::Value) -> Self;

    /// What the form holds.
    fn value(&self) -> &Self::Value;
}

impl<T> IsForm for Form<T> {
    type Value = T;

    fn from_value(value: T) -> Self {
        Self(value)
    }

    fn value(&self) -> &T {
        &self.0
    }
}

/// The crate's [`Text`].
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Text`, which a body named `Text` is read as",
    label = "not `quoll::rest::Text`",
    note = "bring quoll's into scope, `use quoll::rest::Text;`, or name a type of your own otherwise"
)]
pub trait IsText: Sized {
    /// The text holding `text`.
    fn from_string(text: String) -> Self;

    /// The text it holds.
    fn into_string(self) -> String;
}

impl IsText for Text {
    fn from_string(text: String) -> Self {
        Self(text)
    }

    fn into_string(self) -> String {
        self.0
    }
}

/// The crate's [`Bytes`].
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Bytes`, which a body named `Bytes` is read as",
    label = "not `quoll::rest::Bytes`",
    note = "bring quoll's into scope, `use quoll::rest::Bytes;`, or name a type of your own otherwise"
)]
pub trait IsBytes: Sized {
    /// The bytes holding `bytes`.
    fn from_vec(bytes: Vec<u8>) -> Self;

    /// The bytes it holds.
    fn into_vec(self) -> Vec<u8>;
}

impl IsBytes for Bytes {
    fn from_vec(bytes: Vec<u8>) -> Self {
        Self(bytes)
    }

    fn into_vec(self) -> Vec<u8> {
        self.0
    }
}

/// Checks that `T`, a result named `Created`, is the crate's.
pub const fn created<T: IsCreated>() {}

/// Checks that `T`, a body named `Form`, is the crate's.
pub const fn form<T: IsForm>() {}

/// Checks that `T`, a body named `Text`, is the crate's.
pub const fn text<T: IsText>() {}

/// Checks that `T`, a body named `Bytes`, is the crate's.
pub const fn bytes<T: IsBytes>() {}
