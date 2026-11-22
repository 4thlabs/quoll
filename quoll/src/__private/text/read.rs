//! A header's value read as a value of its type: the text a path parameter is read from, for every integer, a float,
//! a `bool`, a `char`, a string or a unit variant, and a newtype around one of them. An `Option` is `None` when the
//! header is missing, and any other type is then an error of its own.

use std::fmt;
use std::str::FromStr;

use serde::de::value::StrDeserializer;
use serde::de::{self, DeserializeOwned, IntoDeserializer, Visitor};

/// Why a header does not read as its type.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum TextError {
    /// The header is missing, and its type is not an `Option`.
    Missing,
    /// The header's text is not a value of its type.
    Invalid(String),
}

impl fmt::Display for TextError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Missing => f.write_str("the value is missing"),
            Self::Invalid(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for TextError {}

impl de::Error for TextError {
    fn custom<T: fmt::Display>(message: T) -> Self {
        Self::Invalid(message.to_string())
    }
}

/// `text`, or `None` for a header that is missing, read as a `T`.
pub(crate) fn from_text<T: DeserializeOwned>(text: Option<&str>) -> Result<T, TextError> {
    T::deserialize(Text(text))
}

/// A deserializer of one piece of text, or of none.
struct Text<'a>(Option<&'a str>);

impl<'a> Text<'a> {
    fn text(&self) -> Result<&'a str, TextError> {
        self.0.ok_or(TextError::Missing)
    }

    /// The text parsed as `T`, named `what` in the error.
    fn parse<T: FromStr>(&self, what: &str) -> Result<T, TextError> {
        self.text()?
            .parse()
            .map_err(|_| TextError::Invalid(format!("expected {what}")))
    }

    /// The error for a type that is not read from text: `Missing` when there is no text, as for any type.
    fn not_text<T>(&self, what: &str) -> Result<T, TextError> {
        self.text()?;

        Err(TextError::Invalid(format!(
            "{what} is not read from one piece of text"
        )))
    }
}

/// Reads each listed method's value with `FromStr`, then gives it to the visitor.
macro_rules! parse {
    ($($method:ident => $visit:ident: $ty:ty, $what:literal;)*) => {
        $(
            fn $method<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
                visitor.$visit(self.parse::<$ty>($what)?)
            }
        )*
    };
}

impl<'de> de::Deserializer<'de> for Text<'_> {
    type Error = TextError;

    fn deserialize_any<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        visitor.visit_str(self.text()?)
    }

    parse! {
        deserialize_bool => visit_bool: bool, "`true` or `false`";
        deserialize_i8 => visit_i8: i8, "an integer";
        deserialize_i16 => visit_i16: i16, "an integer";
        deserialize_i32 => visit_i32: i32, "an integer";
        deserialize_i64 => visit_i64: i64, "an integer";
        deserialize_i128 => visit_i128: i128, "an integer";
        deserialize_u8 => visit_u8: u8, "an integer that is not negative";
        deserialize_u16 => visit_u16: u16, "an integer that is not negative";
        deserialize_u32 => visit_u32: u32, "an integer that is not negative";
        deserialize_u64 => visit_u64: u64, "an integer that is not negative";
        deserialize_u128 => visit_u128: u128, "an integer that is not negative";
        deserialize_f32 => visit_f32: f32, "a number";
        deserialize_f64 => visit_f64: f64, "a number";
        deserialize_char => visit_char: char, "one character";
    }

    fn deserialize_str<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        visitor.visit_str(self.text()?)
    }

    fn deserialize_string<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        visitor.visit_str(self.text()?)
    }

    fn deserialize_identifier<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        visitor.visit_str(self.text()?)
    }

    fn deserialize_bytes<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        visitor.visit_bytes(self.text()?.as_bytes())
    }

    fn deserialize_byte_buf<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        visitor.visit_bytes(self.text()?.as_bytes())
    }

    fn deserialize_option<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        match self.0 {
            Some(_) => visitor.visit_some(self),
            None => visitor.visit_none(),
        }
    }

    fn deserialize_unit<V: Visitor<'de>>(self, _visitor: V) -> Result<V::Value, TextError> {
        self.not_text("`()`")
    }

    fn deserialize_unit_struct<V: Visitor<'de>>(
        self,
        _name: &'static str,
        _visitor: V,
    ) -> Result<V::Value, TextError> {
        self.not_text("a unit struct")
    }

    fn deserialize_newtype_struct<V: Visitor<'de>>(
        self,
        _name: &'static str,
        visitor: V,
    ) -> Result<V::Value, TextError> {
        visitor.visit_newtype_struct(self)
    }

    fn deserialize_seq<V: Visitor<'de>>(self, _visitor: V) -> Result<V::Value, TextError> {
        self.not_text("a sequence")
    }

    fn deserialize_tuple<V: Visitor<'de>>(
        self,
        _len: usize,
        _visitor: V,
    ) -> Result<V::Value, TextError> {
        self.not_text("a tuple")
    }

    fn deserialize_tuple_struct<V: Visitor<'de>>(
        self,
        _name: &'static str,
        _len: usize,
        _visitor: V,
    ) -> Result<V::Value, TextError> {
        self.not_text("a tuple struct")
    }

    fn deserialize_map<V: Visitor<'de>>(self, _visitor: V) -> Result<V::Value, TextError> {
        self.not_text("a map")
    }

    fn deserialize_struct<V: Visitor<'de>>(
        self,
        _name: &'static str,
        _fields: &'static [&'static str],
        _visitor: V,
    ) -> Result<V::Value, TextError> {
        self.not_text("a struct")
    }

    /// A unit variant, named by the text: a variant that holds a value is refused by the enum's own `Deserialize`.
    fn deserialize_enum<V: Visitor<'de>>(
        self,
        _name: &'static str,
        _variants: &'static [&'static str],
        visitor: V,
    ) -> Result<V::Value, TextError> {
        let variant: StrDeserializer<'_, TextError> = self.text()?.into_deserializer();

        visitor.visit_enum(variant)
    }

    fn deserialize_ignored_any<V: Visitor<'de>>(self, visitor: V) -> Result<V::Value, TextError> {
        visitor.visit_unit()
    }
}

#[cfg(test)]
mod tests {
    use serde::Deserialize;

    use super::*;

    #[derive(Debug, PartialEq, Deserialize)]
    #[serde(rename_all = "snake_case")]
    enum Shelf {
        KitchenTop,
        Office,
    }

    #[derive(Debug, PartialEq, Deserialize)]
    enum Shape {
        Circle(f64),
    }

    #[derive(Debug, PartialEq, Deserialize)]
    struct Page(u32);

    #[derive(Debug, PartialEq, Deserialize)]
    struct Point {
        x: i32,
    }

    #[test]
    fn a_scalar_reads_from_its_text() {
        assert_eq!(from_text::<i64>(Some("-3")), Ok(-3));
        assert_eq!(
            from_text::<u128>(Some("340282366920938463463374607431768211455")),
            Ok(u128::MAX)
        );
        assert_eq!(from_text::<f64>(Some("1.5")), Ok(1.5));
        assert_eq!(from_text::<bool>(Some("true")), Ok(true));
        assert_eq!(from_text::<char>(Some("é")), Ok('é'));
        assert_eq!(from_text::<String>(Some(" milk ")), Ok(" milk ".to_owned()));
        assert_eq!(from_text::<String>(Some("")), Ok(String::new()));
        assert_eq!(
            from_text::<Shelf>(Some("kitchen_top")),
            Ok(Shelf::KitchenTop)
        );
        assert_eq!(from_text::<Page>(Some("2")), Ok(Page(2)));
    }

    #[test]
    fn an_option_is_none_when_missing_and_some_when_given() {
        assert_eq!(from_text::<Option<u32>>(None), Ok(None));
        assert_eq!(from_text::<Option<u32>>(Some("7")), Ok(Some(7)));
        assert_eq!(
            from_text::<Option<Shelf>>(Some("office")),
            Ok(Some(Shelf::Office))
        );
        assert_eq!(
            from_text::<Option<u32>>(Some("seven")),
            Err(TextError::Invalid(
                "expected an integer that is not negative".into()
            ))
        );
    }

    #[test]
    fn a_missing_value_of_any_other_type_is_missing() {
        assert_eq!(from_text::<String>(None), Err(TextError::Missing));
        assert_eq!(from_text::<i64>(None), Err(TextError::Missing));
        assert_eq!(from_text::<Shelf>(None), Err(TextError::Missing));
        assert_eq!(from_text::<Page>(None), Err(TextError::Missing));
        assert_eq!(from_text::<Point>(None), Err(TextError::Missing));
    }

    #[test]
    fn text_that_is_not_its_type_is_invalid() {
        assert_eq!(
            from_text::<i64>(Some("two")),
            Err(TextError::Invalid("expected an integer".into()))
        );
        assert_eq!(
            from_text::<u8>(Some("256")),
            Err(TextError::Invalid(
                "expected an integer that is not negative".into()
            ))
        );
        assert_eq!(
            from_text::<bool>(Some("yes")),
            Err(TextError::Invalid("expected `true` or `false`".into()))
        );
        assert!(matches!(
            from_text::<Shelf>(Some("attic")),
            Err(TextError::Invalid(_))
        ));
        assert!(matches!(
            from_text::<Shape>(Some("Circle")),
            Err(TextError::Invalid(_))
        ));
    }

    #[test]
    fn what_is_not_one_piece_of_text_is_refused() {
        assert_eq!(
            from_text::<Point>(Some("x")),
            Err(TextError::Invalid(
                "a struct is not read from one piece of text".into()
            ))
        );
        assert!(matches!(
            from_text::<Vec<u32>>(Some("1")),
            Err(TextError::Invalid(_))
        ));
        assert!(matches!(
            from_text::<(u32, u32)>(Some("1")),
            Err(TextError::Invalid(_))
        ));
        assert!(matches!(
            from_text::<()>(Some("")),
            Err(TextError::Invalid(_))
        ));
    }
}
