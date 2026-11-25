//! The types a contract names that the macro recognizes by their name: `Created`, `Accepted`, `Events`, `JsonLines`,
//! `ByteStream` and `TcpTunnel` as a result, `Form` as a body, `Text` and `Bytes` as either.
//!
//! The macro writes them with the contract's own tokens, so that the user's import is the one that names them. Each
//! trait here is implemented by the crate's type alone: a type of another crate, or of the user's, that has one of
//! these names fails on it, with a message that says so. The checks of [`created`], [`accepted`], [`form`], [`text`],
//! [`bytes`], [`events`], [`json_lines`], [`byte_stream`] and [`tcp_tunnel`] are written whatever the features of
//! `quoll`, so that a contract crate built with neither feature checks them too.
//!
//! [`sized`] checks the type an argument borrows, the `X` of a `&X`, so that the server receives it owned.
//!
//! [`Nullable`] and [`MayBeEmpty`] tell a result's type at compile time, by the impls of the types they name: whether
//! a 204 reads as `null`, and whether a 204 may answer it at all, as the handler's value says, whatever wraps it.

use std::borrow::Cow;
use std::marker::PhantomData;

use serde::de::DeserializeOwned;

use crate::{
    Accepted, Allows, ByteStream, Bytes, Created, Error, Events, Form, JsonLines, NoBody, Result,
    TcpTunnel, Text,
};

pub use crate::stream::Items;

/// Whether a result's type `T` reads an empty answer, a 204 or a 205, as `null`: [`IS`](Nullable::IS) is `true` for
/// a value that may be empty, of the one list `quoll::Allows` holds, and that is read as JSON, `()` and an `Option<T>`,
/// by the impl whose bounds say so, and `false` for any other type, by [`NotNullable`], whose constant a path reaches
/// only where no impl of the type's own has one.
#[derive(Debug)]
pub struct Nullable<T>(PhantomData<fn() -> T>);

impl<T> Nullable<T>
where
    NoBody: Allows<T>,
    T: DeserializeOwned,
{
    /// A value that may be empty, read as JSON: `null` when empty.
    pub const IS: bool = true;
}

/// Whether the value `T` of a handler's response may be empty, a result a 204 answers: [`IS`](MayBeEmpty::IS) is
/// `true` for each type `quoll::Allows` gives a 204, the one list of them, by the impl whose bound says so, and `false`
/// for any other, by [`NotEmpty`]. The OpenAPI document lists a 204 for it, whatever the result wraps it in, a
/// `Created<T>` or an `Accepted<T>` among them.
#[derive(Debug)]
pub struct MayBeEmpty<T>(PhantomData<fn() -> T>);

impl<T> MayBeEmpty<T>
where
    NoBody: Allows<T>,
{
    /// A value that may be empty.
    pub const IS: bool = true;
}

/// The constant of any type that is not empty, which the code of a contract brings into scope.
pub trait NotEmpty {
    /// A value.
    const IS: bool = false;
}

impl<T> NotEmpty for MayBeEmpty<T> {}

/// The constant of any type but an `Option<T>` and `()`, which the code of a contract brings into scope.
pub trait NotNullable {
    /// A type that an empty answer is not.
    const IS: bool = false;
}

impl<T> NotNullable for Nullable<T> {}

/// The crate's [`Created`], whose `T` the server answers with a 201.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Created`, which a result named `Created` is read as",
    label = "not `quoll::Created`",
    note = "bring quoll's into scope, `use quoll::Created;`, or name a type of your own otherwise"
)]
pub trait IsCreated: Sized {
    /// The `T` of `Created<T>`, the body.
    type Value;
}

impl<T> IsCreated for Created<T> {
    type Value = T;
}

/// The crate's [`Accepted`], whose `T` the server answers with a 202.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Accepted`, which a result named `Accepted` is read as",
    label = "not `quoll::Accepted`",
    note = "bring quoll's into scope, `use quoll::Accepted;`, or name a type of your own otherwise"
)]
pub trait IsAccepted: Sized {
    /// The `T` of `Accepted<T>`, the body.
    type Value;
}

impl<T> IsAccepted for Accepted<T> {
    type Value = T;
}

/// The crate's [`Form`], whose `T` is read and written as a form.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Form`, which a body named `Form` is read as",
    label = "not `quoll::Form`",
    note = "bring quoll's into scope, `use quoll::Form;`, or name a type of your own otherwise"
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
    message = "`{Self}` is not quoll's `Text`, which a body or a result named `Text` is read as",
    label = "not `quoll::Text`",
    note = "bring quoll's into scope, `use quoll::Text;`, or name a type of your own otherwise"
)]
pub trait IsText: Sized {
    /// The text holding `text`.
    fn from_string(text: String) -> Self;

    /// The text it holds.
    fn into_string(self) -> String;

    /// The text it holds, borrowed.
    fn as_str(&self) -> &str;
}

impl IsText for Text {
    fn from_string(text: String) -> Self {
        Self(text)
    }

    fn into_string(self) -> String {
        self.0
    }

    fn as_str(&self) -> &str {
        &self.0
    }
}

/// The crate's [`Bytes`].
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Bytes`, which a body or a result named `Bytes` is read as",
    label = "not `quoll::Bytes`",
    note = "bring quoll's into scope, `use quoll::Bytes;`, or name a type of your own otherwise"
)]
pub trait IsBytes: Sized {
    /// The bytes holding `bytes`, of `content_type`.
    fn from_parts(bytes: Vec<u8>, content_type: Cow<'static, str>) -> Self;

    /// The bytes it holds, and their content type.
    fn into_parts(self) -> (Vec<u8>, Cow<'static, str>);

    /// The bytes it holds, and their content type, borrowed.
    fn parts(&self) -> (&[u8], &Cow<'static, str>);
}

impl IsBytes for Bytes {
    fn from_parts(bytes: Vec<u8>, content_type: Cow<'static, str>) -> Self {
        Self {
            bytes,
            content_type,
        }
    }

    fn into_parts(self) -> (Vec<u8>, Cow<'static, str>) {
        (self.bytes, self.content_type)
    }

    fn parts(&self) -> (&[u8], &Cow<'static, str>) {
        (&self.bytes, &self.content_type)
    }
}

/// A stream result of the crate's, [`Events`] or [`JsonLines`]: its items, which the server sends and the client
/// reads.
pub trait Streamed: Sized {
    /// The `T` of `Events<T>` or `JsonLines<T>`, the value of each item.
    type Item;

    /// The stream result of `items`, as the client reads them.
    fn from_items(items: Items<Self::Item>) -> Self;

    /// The items, as the server sends them.
    fn into_items(self) -> Items<Self::Item>;
}

impl<T> Streamed for Events<T> {
    type Item = T;

    fn from_items(items: Items<T>) -> Self {
        Self { items }
    }

    fn into_items(self) -> Items<T> {
        self.items
    }
}

impl<T> Streamed for JsonLines<T> {
    type Item = T;

    fn from_items(items: Items<T>) -> Self {
        Self { items }
    }

    fn into_items(self) -> Items<T> {
        self.items
    }
}

/// The crate's [`Events`].
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `Events`, which a result named `Events` is read as",
    label = "not `quoll::Events`",
    note = "bring quoll's into scope, `use quoll::Events;`, or name a type of your own otherwise"
)]
pub trait IsEvents: Streamed {}

impl<T> IsEvents for Events<T> {}

/// The crate's [`JsonLines`].
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `JsonLines`, which a result named `JsonLines` is read as",
    label = "not `quoll::JsonLines`",
    note = "bring quoll's into scope, `use quoll::JsonLines;`, or name a type of your own otherwise"
)]
pub trait IsJsonLines: Streamed {}

impl<T> IsJsonLines for JsonLines<T> {}

/// The chunks of a [`ByteStream`], their content type and their length, as the server sends them.
pub type ByteParts = (Items<Vec<u8>>, Cow<'static, str>, Option<u64>);

/// The crate's [`ByteStream`]: its chunks, their content type and their length, which the server sends and the client
/// reads.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `ByteStream`, which a result named `ByteStream` is read as",
    label = "not `quoll::ByteStream`",
    note = "bring quoll's into scope, `use quoll::ByteStream;`, or name a type of your own otherwise"
)]
pub trait IsByteStream: Sized {
    /// The bytes of `chunks`, of `content_type`, `length` bytes in all when it is known, as the client reads them.
    fn from_parts(
        chunks: Items<Vec<u8>>,
        content_type: Cow<'static, str>,
        length: Option<u64>,
    ) -> Self;

    /// The chunks, their content type and their length, as the server sends them.
    ///
    /// # Errors
    ///
    /// When a chunk was read past the length before the stream was sent: what is left of it is no length.
    fn into_parts(self) -> Result<ByteParts>;
}

impl IsByteStream for ByteStream {
    fn from_parts(
        chunks: Items<Vec<u8>>,
        content_type: Cow<'static, str>,
        length: Option<u64>,
    ) -> Self {
        Self {
            chunks,
            content_type,
            length,
            read_past: false,
        }
    }

    fn into_parts(self) -> Result<ByteParts> {
        if self.read_past {
            return Err(Error::internal(
                "a `ByteStream` was read past its length before it was sent: its chunks hold more bytes than it said",
            ));
        }

        Ok((self.chunks, self.content_type, self.length))
    }
}

/// The crate's [`TcpTunnel`]: the connection a 101 switched to `tcp`, which the client gives.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `TcpTunnel`, which a result named `TcpTunnel` is read as",
    label = "not `quoll::TcpTunnel`",
    note = "bring quoll's into scope, `use quoll::TcpTunnel;`, or name a type of your own otherwise"
)]
pub trait IsTcpTunnel: Sized {
    /// The tunnel of the connection reqwest gave back, of the content type of its 101.
    #[cfg(feature = "client")]
    fn from_upgraded(upgraded: reqwest::Upgraded, content_type: Cow<'static, str>) -> Self;
}

impl IsTcpTunnel for TcpTunnel {
    #[cfg(feature = "client")]
    fn from_upgraded(upgraded: reqwest::Upgraded, content_type: Cow<'static, str>) -> Self {
        Self::upgraded(upgraded, content_type)
    }
}

/// Checks that `T`, a result named `Created`, is the crate's.
pub const fn created<T: IsCreated>() {}

/// Checks that a type an argument borrows, the `X` of a `&X`, has a size, so that the server receives it owned.
pub const fn sized<T>() {}

/// Checks that `T`, a result named `Accepted`, is the crate's.
pub const fn accepted<T: IsAccepted>() {}

/// Checks that `T`, a body named `Form`, is the crate's.
pub const fn form<T: IsForm>() {}

/// Checks that `T`, a body or a result named `Text`, is the crate's.
pub const fn text<T: IsText>() {}

/// Checks that `T`, a body or a result named `Bytes`, is the crate's.
pub const fn bytes<T: IsBytes>() {}

/// Checks that `T`, a result named `Events`, is the crate's.
pub const fn events<T: IsEvents>() {}

/// Checks that `T`, a result named `JsonLines`, is the crate's.
pub const fn json_lines<T: IsJsonLines>() {}

/// Checks that `T`, a result named `ByteStream`, is the crate's.
pub const fn byte_stream<T: IsByteStream>() {}

/// Checks that `T`, a result named `TcpTunnel`, is the crate's.
pub const fn tcp_tunnel<T: IsTcpTunnel>() {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_value_that_may_be_empty_is_null_when_it_is_read_as_json() {
        // The one list, `quoll::Allows`'s: its JSON values are `null` when empty, the others empty of their own.
        // Checked as the test is built.
        const { assert!(<MayBeEmpty<()>>::IS && <Nullable<()>>::IS) };
        const { assert!(<MayBeEmpty<Option<u8>>>::IS && <Nullable<Option<u8>>>::IS) };
        const { assert!(<MayBeEmpty<Text>>::IS && !<Nullable<Text>>::IS) };
        const { assert!(<MayBeEmpty<Bytes>>::IS && !<Nullable<Bytes>>::IS) };
        const { assert!(<MayBeEmpty<Events<u8>>>::IS && !<Nullable<Events<u8>>>::IS) };
        const { assert!(<MayBeEmpty<JsonLines<u8>>>::IS && !<Nullable<JsonLines<u8>>>::IS) };
        const { assert!(<MayBeEmpty<ByteStream>>::IS && !<Nullable<ByteStream>>::IS) };
        const { assert!(!<MayBeEmpty<u8>>::IS && !<Nullable<u8>>::IS) };
        const { assert!(!<MayBeEmpty<serde_json::Value>>::IS && !<Nullable<serde_json::Value>>::IS) };
    }
}
