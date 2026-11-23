//! The types a contract names that the macro recognizes by their name: `Created`, `Accepted`, `Events`, `JsonLines`
//! and `ByteStream` as a result, `Form` as a body, `Text` and `Bytes` as either.
//!
//! The macro writes them with the contract's own tokens, so that the user's import is the one that names them. Each
//! trait here is implemented by the crate's type alone: a type of another crate, or of the user's, that has one of
//! these names fails on it, with a message that says so. The checks of [`created`], [`accepted`], [`form`], [`text`],
//! [`bytes`], [`events`], [`json_lines`] and [`byte_stream`] are written whatever the features of `quoll`, so that a contract crate
//! built with neither feature checks them too.
//!
//! [`Nullable`] and [`MayBeEmpty`] tell a result's type at compile time, by the impls of the types they name: whether
//! a 204 reads as `null`, and whether a 204 may answer it at all, which a `Created<T>` or an `Accepted<T>` may not.

use std::borrow::Cow;
use std::marker::PhantomData;

use crate::rest::{Accepted, ByteStream, Bytes, Created, Events, Form, JsonLines, Text};

pub use crate::rest::stream::Items;

/// Whether a result's type `T` reads an empty answer, a 204 or a 205, as `null`: [`IS`](Nullable::IS) is `true` for
/// an `Option<T>` and `()` alone, by their impls, and `false` for any other type, by [`NotNullable`], whose constant a
/// path reaches only where no impl of the type's own has one. The client reads such an answer so, and the OpenAPI
/// document lists a 204 for it.
#[derive(Debug)]
pub struct Nullable<T>(PhantomData<fn() -> T>);

impl<T> Nullable<Option<T>> {
    /// An `Option<T>`, `None` when empty.
    pub const IS: bool = true;
}

impl Nullable<()> {
    /// `()`, which `null` is.
    pub const IS: bool = true;
}

/// Whether a type `T` may be empty, a result a 204 answers as `rest::Allows` lists them: [`IS`](MayBeEmpty::IS) is
/// `true` for `()`, an `Option<T>`, `Text`, `Bytes`, `Events<T>`, `JsonLines<T>` and `ByteStream`, by their impls, and
/// `false` for any other, by [`NotEmpty`]. The `T` of a `Created<T>` or an `Accepted<T>` is none of them: its status
/// carries a value, which a 204 would not.
#[derive(Debug)]
pub struct MayBeEmpty<T>(PhantomData<fn() -> T>);

/// Gives `MayBeEmpty` of each result that may be empty its constant, `T` its parameter where it has one.
macro_rules! may_be_empty {
    ($($ty:ty $(=> $param:ident)?),* $(,)?) => {
        $(
            impl$(<$param>)? MayBeEmpty<$ty> {
                /// A result that may be empty.
                pub const IS: bool = true;
            }
        )*
    };
}

may_be_empty! {
    (),
    Option<T> => T,
    Text,
    Bytes,
    Events<T> => T,
    JsonLines<T> => T,
    ByteStream,
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
    label = "not `quoll::rest::Created`",
    note = "bring quoll's into scope, `use quoll::rest::Created;`, or name a type of your own otherwise"
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
    label = "not `quoll::rest::Accepted`",
    note = "bring quoll's into scope, `use quoll::rest::Accepted;`, or name a type of your own otherwise"
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
    message = "`{Self}` is not quoll's `Text`, which a body or a result named `Text` is read as",
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
    message = "`{Self}` is not quoll's `Bytes`, which a body or a result named `Bytes` is read as",
    label = "not `quoll::rest::Bytes`",
    note = "bring quoll's into scope, `use quoll::rest::Bytes;`, or name a type of your own otherwise"
)]
pub trait IsBytes: Sized {
    /// The bytes holding `bytes`, of `content_type`.
    fn from_parts(bytes: Vec<u8>, content_type: Cow<'static, str>) -> Self;

    /// The bytes it holds, and their content type.
    fn into_parts(self) -> (Vec<u8>, Cow<'static, str>);
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
    label = "not `quoll::rest::Events`",
    note = "bring quoll's into scope, `use quoll::rest::Events;`, or name a type of your own otherwise"
)]
pub trait IsEvents: Streamed {}

impl<T> IsEvents for Events<T> {}

/// The crate's [`JsonLines`].
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `JsonLines`, which a result named `JsonLines` is read as",
    label = "not `quoll::rest::JsonLines`",
    note = "bring quoll's into scope, `use quoll::rest::JsonLines;`, or name a type of your own otherwise"
)]
pub trait IsJsonLines: Streamed {}

impl<T> IsJsonLines for JsonLines<T> {}

/// The crate's [`ByteStream`]: its chunks and their content type, which the server sends and the client reads.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not quoll's `ByteStream`, which a result named `ByteStream` is read as",
    label = "not `quoll::rest::ByteStream`",
    note = "bring quoll's into scope, `use quoll::rest::ByteStream;`, or name a type of your own otherwise"
)]
pub trait IsByteStream: Sized {
    /// The bytes of `chunks`, of `content_type`, as the client reads them.
    fn from_parts(chunks: Items<Vec<u8>>, content_type: Cow<'static, str>) -> Self;

    /// The chunks and their content type, as the server sends them.
    fn into_parts(self) -> (Items<Vec<u8>>, Cow<'static, str>);
}

impl IsByteStream for ByteStream {
    fn from_parts(chunks: Items<Vec<u8>>, content_type: Cow<'static, str>) -> Self {
        Self {
            chunks,
            content_type,
        }
    }

    fn into_parts(self) -> (Items<Vec<u8>>, Cow<'static, str>) {
        (self.chunks, self.content_type)
    }
}

/// Checks that `T`, a result named `Created`, is the crate's.
pub const fn created<T: IsCreated>() {}

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
