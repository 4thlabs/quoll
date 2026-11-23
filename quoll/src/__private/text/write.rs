//! A value written as the text of one path parameter, one header or one value of a joined list: what axum's `Path`
//! reads back, for every integer, a float, a `bool`, a `char`, a string or a unit variant, and a newtype or an
//! `Option` around one of them.

use std::fmt;

use serde::ser::{self, Impossible, Serialize};

/// The text of `value`, `Some(None)` for a `None`; `None` when it is not written as one piece of text: a sequence, a
/// map, a struct.
pub(crate) fn to_text<T: Serialize + ?Sized>(value: &T) -> Option<Option<String>> {
    match value.serialize(Text) {
        Ok(Written::Text(text)) => Some(Some(text)),
        Ok(Written::Nothing) => Some(None),
        Err(NotText) => None,
    }
}

/// What `Text` writes: a value's text, or nothing for a `None`.
enum Written {
    Text(String),
    Nothing,
}

/// A serializer that writes a scalar as its text and refuses anything else.
struct Text;

/// What `Text` refuses: a value that is not one piece of text.
#[derive(Debug)]
struct NotText;

impl fmt::Display for NotText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("the value is not written as text")
    }
}

impl std::error::Error for NotText {}

impl ser::Error for NotText {
    fn custom<T: fmt::Display>(_message: T) -> Self {
        Self
    }
}

/// Writes each listed method as `value.to_string()`.
macro_rules! display {
    ($($method:ident: $ty:ty),* $(,)?) => {
        $(
            fn $method(self, value: $ty) -> Result<Written, NotText> {
                Ok(Written::Text(value.to_string()))
            }
        )*
    };
}

impl ser::Serializer for Text {
    type Ok = Written;
    type Error = NotText;
    type SerializeSeq = Impossible<Written, NotText>;
    type SerializeTuple = Impossible<Written, NotText>;
    type SerializeTupleStruct = Impossible<Written, NotText>;
    type SerializeTupleVariant = Impossible<Written, NotText>;
    type SerializeMap = Impossible<Written, NotText>;
    type SerializeStruct = Impossible<Written, NotText>;
    type SerializeStructVariant = Impossible<Written, NotText>;

    display! {
        serialize_bool: bool,
        serialize_i8: i8,
        serialize_i16: i16,
        serialize_i32: i32,
        serialize_i64: i64,
        serialize_i128: i128,
        serialize_u8: u8,
        serialize_u16: u16,
        serialize_u32: u32,
        serialize_u64: u64,
        serialize_u128: u128,
        serialize_f32: f32,
        serialize_f64: f64,
        serialize_char: char,
        serialize_str: &str,
    }

    fn serialize_bytes(self, _value: &[u8]) -> Result<Written, NotText> {
        Err(NotText)
    }

    fn serialize_none(self) -> Result<Written, NotText> {
        Ok(Written::Nothing)
    }

    fn serialize_some<T: Serialize + ?Sized>(self, value: &T) -> Result<Written, NotText> {
        value.serialize(self)
    }

    fn serialize_unit(self) -> Result<Written, NotText> {
        Err(NotText)
    }

    fn serialize_unit_struct(self, _name: &'static str) -> Result<Written, NotText> {
        Err(NotText)
    }

    fn serialize_unit_variant(
        self,
        _name: &'static str,
        _index: u32,
        variant: &'static str,
    ) -> Result<Written, NotText> {
        Ok(Written::Text(variant.to_owned()))
    }

    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        value: &T,
    ) -> Result<Written, NotText> {
        value.serialize(self)
    }

    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _value: &T,
    ) -> Result<Written, NotText> {
        Err(NotText)
    }

    fn serialize_seq(self, _len: Option<usize>) -> Result<Self::SerializeSeq, NotText> {
        Err(NotText)
    }

    fn serialize_tuple(self, _len: usize) -> Result<Self::SerializeTuple, NotText> {
        Err(NotText)
    }

    fn serialize_tuple_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleStruct, NotText> {
        Err(NotText)
    }

    fn serialize_tuple_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleVariant, NotText> {
        Err(NotText)
    }

    fn serialize_map(self, _len: Option<usize>) -> Result<Self::SerializeMap, NotText> {
        Err(NotText)
    }

    fn serialize_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStruct, NotText> {
        Err(NotText)
    }

    fn serialize_struct_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStructVariant, NotText> {
        Err(NotText)
    }
}
