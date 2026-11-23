//! The fixed fields of a body, `#[body("stream" = true)]` on a route: what the generated code names them by, whatever
//! the features, the adapter that adds them to the body the client writes, the check that a body the server reads
//! holds them, and whether two routes of one method and path are told apart by them, at compile time.
//!
//! The adapter and the check go through serde alone, so that any self-describing format of serde's takes them, JSON
//! and forms today: the body is written once, with its fields, and read twice, once for its fields and once into the
//! handler's type, without an intermediate value.

use std::fmt;

use serde::de::{DeserializeSeed, Deserializer, IgnoredAny, MapAccess, Visitor};
use serde::ser::{self, Impossible, SerializeMap, SerializeStruct, Serializer};
use serde::{Deserialize, Serialize};

/// A field a route fixes in its body, `"stream" = true`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Field {
    /// Its name, as the body names it.
    pub name: &'static str,
    /// Its value.
    pub value: Value,
}

/// The value of a fixed field: a bool, a number or a string, as the contract writes it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Value {
    /// `true` or `false`.
    Bool(bool),
    /// An integer an `i64` holds.
    Int(i64),
    /// An integer above `i64::MAX`.
    Uint(u64),
    /// A number with a fraction or an exponent.
    Float(f64),
    /// A string.
    Str(&'static str),
}

impl Value {
    /// Whether it tells a route apart from one that fixes `other` for the same field: both of one kind, numbers
    /// compared as numbers, and different. Values of two kinds do not, as a form writes `true` and `"true"` alike.
    pub const fn differs_from(self, other: Self) -> bool {
        match (self, other) {
            (Self::Bool(a), Self::Bool(b)) => a != b,
            (Self::Str(a), Self::Str(b)) => !same(a, b),
            (Self::Int(a), Self::Int(b)) => a != b,
            (Self::Uint(a), Self::Uint(b)) => a != b,
            // A `Uint` is above `i64::MAX`, which no `Int` reaches.
            (Self::Int(_), Self::Uint(_)) | (Self::Uint(_), Self::Int(_)) => true,
            (Self::Float(a), Self::Float(b)) => a != b,
            #[expect(clippy::cast_precision_loss, reason = "a float is compared as a float")]
            (Self::Int(a), Self::Float(b)) | (Self::Float(b), Self::Int(a)) => a as f64 != b,
            #[expect(clippy::cast_precision_loss, reason = "a float is compared as a float")]
            (Self::Uint(a), Self::Float(b)) | (Self::Float(b), Self::Uint(a)) => a as f64 != b,
            _ => false,
        }
    }
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Bool(value) => write!(f, "{value}"),
            Self::Int(value) => write!(f, "{value}"),
            Self::Uint(value) => write!(f, "{value}"),
            Self::Float(value) => write!(f, "{value:?}"),
            Self::Str(value) => write!(f, "{value:?}"),
        }
    }
}

impl Serialize for Value {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match *self {
            Self::Bool(value) => serializer.serialize_bool(value),
            Self::Int(value) => serializer.serialize_i64(value),
            Self::Uint(value) => serializer.serialize_u64(value),
            Self::Float(value) => serializer.serialize_f64(value),
            Self::Str(value) => serializer.serialize_str(value),
        }
    }
}

/// Whether a route that fixes `first` is told apart from one that fixes `second`, on one method and path: a field
/// both fix, to values that differ.
pub const fn tell_apart(first: &[Field], second: &[Field]) -> bool {
    let mut i = 0;

    while i < first.len() {
        let mut j = 0;

        while j < second.len() {
            if same(first[i].name, second[j].name) && first[i].value.differs_from(second[j].value) {
                return true;
            }
            j += 1;
        }
        i += 1;
    }

    false
}

/// Whether two strings are one.
const fn same(first: &str, second: &str) -> bool {
    let (first, second) = (first.as_bytes(), second.as_bytes());
    if first.len() != second.len() {
        return false;
    }

    let mut i = 0;
    while i < first.len() {
        if first[i] != second[i] {
            return false;
        }
        i += 1;
    }

    true
}

/// The fields as a message shows them: `"stream": true, "n": 1`.
#[cfg(feature = "server")]
pub(crate) fn shown(fields: &[Field]) -> String {
    let fields: Vec<String> = fields
        .iter()
        .map(|field| format!("{:?}: {}", field.name, field.value))
        .collect();

    fields.join(", ")
}

/// A body that serializes as `value` with `fields` added at its top: in the struct or the map `value` serializes
/// as, or in what an `Option` or a newtype holds, a field of `value` of the same name left out, as the contract wins.
/// Any other value is refused when it is written, with a message that says so.
#[derive(Debug)]
pub struct With<'a, T: ?Sized> {
    value: &'a T,
    fields: &'static [Field],
}

impl<'a, T: ?Sized> With<'a, T> {
    /// `value`, with `fields` added when it serializes.
    pub fn new(value: &'a T, fields: &'static [Field]) -> Self {
        Self { value, fields }
    }
}

impl<T: Serialize + ?Sized> Serialize for With<'_, T> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.value.serialize(Adder {
            inner: serializer,
            fields: self.fields,
        })
    }
}

/// The serializer of [`With`]: the format's own, with the fields added at the top of a struct or a map.
struct Adder<S> {
    inner: S,
    fields: &'static [Field],
}

/// The error of a body that is not a struct or a map, `what`.
fn refused<E: ser::Error>(what: &str) -> E {
    E::custom(format!(
        "a route with fixed fields, `#[body(...)]`, adds them to a body that is a struct or a map, and this one is {what}"
    ))
}

/// The methods of [`Adder`] for a value that is not a struct or a map, each refused with what it is.
macro_rules! refuse {
    ($($method:ident($($arg:ty),*) $what:literal;)*) => {
        $(
            fn $method(self, $(_: $arg),*) -> Result<Self::Ok, Self::Error> {
                Err(refused($what))
            }
        )*
    };
}

impl<S: Serializer> Serializer for Adder<S> {
    type Ok = S::Ok;
    type Error = S::Error;
    type SerializeSeq = Impossible<S::Ok, S::Error>;
    type SerializeTuple = Impossible<S::Ok, S::Error>;
    type SerializeTupleStruct = Impossible<S::Ok, S::Error>;
    type SerializeTupleVariant = Impossible<S::Ok, S::Error>;
    type SerializeMap = AddMap<S::SerializeMap>;
    type SerializeStruct = AddStruct<S::SerializeStruct>;
    type SerializeStructVariant = Impossible<S::Ok, S::Error>;

    refuse! {
        serialize_bool(bool) "a bool";
        serialize_i8(i8) "a number";
        serialize_i16(i16) "a number";
        serialize_i32(i32) "a number";
        serialize_i64(i64) "a number";
        serialize_i128(i128) "a number";
        serialize_u8(u8) "a number";
        serialize_u16(u16) "a number";
        serialize_u32(u32) "a number";
        serialize_u64(u64) "a number";
        serialize_u128(u128) "a number";
        serialize_f32(f32) "a number";
        serialize_f64(f64) "a number";
        serialize_char(char) "a character";
        serialize_str(&str) "a string";
        serialize_bytes(&[u8]) "bytes";
        serialize_none() "`None`";
        serialize_unit() "`()`";
        serialize_unit_struct(&'static str) "a unit struct";
        serialize_unit_variant(&'static str, u32, &'static str) "a unit variant";
    }

    fn serialize_some<T: Serialize + ?Sized>(self, value: &T) -> Result<S::Ok, S::Error> {
        value.serialize(self)
    }

    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        value: &T,
    ) -> Result<S::Ok, S::Error> {
        value.serialize(self)
    }

    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _value: &T,
    ) -> Result<S::Ok, S::Error> {
        Err(refused("an enum's variant, under its name"))
    }

    fn serialize_seq(self, _len: Option<usize>) -> Result<Self::SerializeSeq, S::Error> {
        Err(refused("a sequence"))
    }

    fn serialize_tuple(self, _len: usize) -> Result<Self::SerializeTuple, S::Error> {
        Err(refused("a tuple"))
    }

    fn serialize_tuple_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleStruct, S::Error> {
        Err(refused("a tuple struct"))
    }

    fn serialize_tuple_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleVariant, S::Error> {
        Err(refused("an enum's variant, under its name"))
    }

    fn serialize_map(self, len: Option<usize>) -> Result<Self::SerializeMap, S::Error> {
        // The length counts a field of the map's own that the fixed one replaces twice: JSON and forms do not read it.
        let mut inner = self
            .inner
            .serialize_map(len.map(|len| len + self.fields.len()))?;

        for field in self.fields {
            inner.serialize_entry(field.name, &field.value)?;
        }

        Ok(AddMap {
            inner,
            fields: self.fields,
            skipping: false,
        })
    }

    fn serialize_struct(
        self,
        name: &'static str,
        len: usize,
    ) -> Result<Self::SerializeStruct, S::Error> {
        // As for a map, a field the fixed one replaces is counted twice.
        let mut inner = self.inner.serialize_struct(name, len + self.fields.len())?;

        for field in self.fields {
            inner.serialize_field(field.name, &field.value)?;
        }

        Ok(AddStruct {
            inner,
            fields: self.fields,
        })
    }

    fn serialize_struct_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStructVariant, S::Error> {
        Err(refused("an enum's variant, under its name"))
    }
}

/// Whether `name` is one of `fields`.
fn is_fixed(fields: &[Field], name: &str) -> bool {
    fields.iter().any(|field| field.name == name)
}

/// A struct the fields are added to, which leaves out a field of its own of the same name.
#[derive(Debug)]
struct AddStruct<S> {
    inner: S,
    fields: &'static [Field],
}

impl<S: SerializeStruct> SerializeStruct for AddStruct<S> {
    type Ok = S::Ok;
    type Error = S::Error;

    fn serialize_field<T: Serialize + ?Sized>(
        &mut self,
        key: &'static str,
        value: &T,
    ) -> Result<(), S::Error> {
        if is_fixed(self.fields, key) {
            return Ok(());
        }

        self.inner.serialize_field(key, value)
    }

    fn skip_field(&mut self, key: &'static str) -> Result<(), S::Error> {
        if is_fixed(self.fields, key) {
            return Ok(());
        }

        self.inner.skip_field(key)
    }

    fn end(self) -> Result<S::Ok, S::Error> {
        self.inner.end()
    }
}

/// A map the fields are added to, which leaves out an entry of its own whose key is one of their names.
#[derive(Debug)]
struct AddMap<S> {
    inner: S,
    fields: &'static [Field],
    /// Whether the value to come is the one of a key left out.
    skipping: bool,
}

impl<S: SerializeMap> SerializeMap for AddMap<S> {
    type Ok = S::Ok;
    type Error = S::Error;

    fn serialize_key<T: Serialize + ?Sized>(&mut self, key: &T) -> Result<(), S::Error> {
        self.skipping = key.serialize(KeyName(self.fields)).unwrap_or(false);
        if self.skipping {
            return Ok(());
        }

        self.inner.serialize_key(key)
    }

    fn serialize_value<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<(), S::Error> {
        if std::mem::take(&mut self.skipping) {
            return Ok(());
        }

        self.inner.serialize_value(value)
    }

    fn serialize_entry<K: Serialize + ?Sized, V: Serialize + ?Sized>(
        &mut self,
        key: &K,
        value: &V,
    ) -> Result<(), S::Error> {
        if key.serialize(KeyName(self.fields)).unwrap_or(false) {
            return Ok(());
        }

        self.inner.serialize_entry(key, value)
    }

    fn end(self) -> Result<S::Ok, S::Error> {
        self.inner.end()
    }
}

/// A serializer that tells whether a map's key is the name of one of the fields: a string, a character or a unit
/// variant, as a format writes it as text; any other key is not.
struct KeyName(&'static [Field]);

/// The error of [`KeyName`], which a key that is not one of the fields never reaches the format with.
#[derive(Debug)]
struct NotAName;

impl fmt::Display for NotAName {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("not a name")
    }
}

impl std::error::Error for NotAName {}

impl ser::Error for NotAName {
    fn custom<T: fmt::Display>(_msg: T) -> Self {
        Self
    }
}

/// The methods of [`KeyName`] for a key that is not text: none of the fields' names.
macro_rules! not_a_name {
    ($($method:ident($($arg:ty),*);)*) => {
        $(
            fn $method(self, $(_: $arg),*) -> Result<bool, NotAName> {
                Ok(false)
            }
        )*
    };
}

impl Serializer for KeyName {
    type Ok = bool;
    type Error = NotAName;
    type SerializeSeq = Impossible<bool, NotAName>;
    type SerializeTuple = Impossible<bool, NotAName>;
    type SerializeTupleStruct = Impossible<bool, NotAName>;
    type SerializeTupleVariant = Impossible<bool, NotAName>;
    type SerializeMap = Impossible<bool, NotAName>;
    type SerializeStruct = Impossible<bool, NotAName>;
    type SerializeStructVariant = Impossible<bool, NotAName>;

    not_a_name! {
        serialize_bool(bool);
        serialize_i8(i8);
        serialize_i16(i16);
        serialize_i32(i32);
        serialize_i64(i64);
        serialize_i128(i128);
        serialize_u8(u8);
        serialize_u16(u16);
        serialize_u32(u32);
        serialize_u64(u64);
        serialize_u128(u128);
        serialize_f32(f32);
        serialize_f64(f64);
        serialize_bytes(&[u8]);
        serialize_none();
        serialize_unit();
        serialize_unit_struct(&'static str);
    }

    fn serialize_char(self, value: char) -> Result<bool, NotAName> {
        Ok(is_fixed(self.0, value.encode_utf8(&mut [0; 4])))
    }

    fn serialize_str(self, value: &str) -> Result<bool, NotAName> {
        Ok(is_fixed(self.0, value))
    }

    fn serialize_unit_variant(
        self,
        _name: &'static str,
        _index: u32,
        variant: &'static str,
    ) -> Result<bool, NotAName> {
        Ok(is_fixed(self.0, variant))
    }

    fn serialize_some<T: Serialize + ?Sized>(self, value: &T) -> Result<bool, NotAName> {
        value.serialize(self)
    }

    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        value: &T,
    ) -> Result<bool, NotAName> {
        value.serialize(self)
    }

    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _value: &T,
    ) -> Result<bool, NotAName> {
        Ok(false)
    }

    fn serialize_seq(self, _len: Option<usize>) -> Result<Self::SerializeSeq, NotAName> {
        Err(NotAName)
    }

    fn serialize_tuple(self, _len: usize) -> Result<Self::SerializeTuple, NotAName> {
        Err(NotAName)
    }

    fn serialize_tuple_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleStruct, NotAName> {
        Err(NotAName)
    }

    fn serialize_tuple_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeTupleVariant, NotAName> {
        Err(NotAName)
    }

    fn serialize_map(self, _len: Option<usize>) -> Result<Self::SerializeMap, NotAName> {
        Err(NotAName)
    }

    fn serialize_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStruct, NotAName> {
        Err(NotAName)
    }

    fn serialize_struct_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStructVariant, NotAName> {
        Err(NotAName)
    }
}

/// Whether the body `deserializer` reads is a struct or a map that holds each of `fields` once, of its value, read
/// as a struct of those fields alone would read them: any other key is skipped. A body that does not read so, a field
/// missing, given twice or of another value, does not hold them.
pub fn holds<'de, D: Deserializer<'de>>(deserializer: D, fields: &'static [Field]) -> bool {
    deserializer.deserialize_map(Holds(fields)).unwrap_or(false)
}

/// The visitor of [`holds`].
struct Holds(&'static [Field]);

impl<'de> Visitor<'de> for Holds {
    type Value = bool;

    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("a struct or a map")
    }

    fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<bool, A::Error> {
        let mut seen = vec![false; self.0.len()];
        let mut holds = true;

        while let Some(key) = map.next_key::<String>()? {
            let Some(index) = self.0.iter().position(|field| field.name == key) else {
                map.next_value::<IgnoredAny>()?;
                continue;
            };

            if std::mem::replace(&mut seen[index], true) {
                holds = false;
                map.next_value::<IgnoredAny>()?;
                continue;
            }
            if !map.next_value_seed(Equals(self.0[index].value))? {
                holds = false;
            }
        }

        Ok(holds && seen.into_iter().all(|seen| seen))
    }
}

/// A field's value, read as its type and compared to `.0`.
struct Equals(Value);

impl<'de> DeserializeSeed<'de> for Equals {
    type Value = bool;

    fn deserialize<D: Deserializer<'de>>(self, deserializer: D) -> Result<bool, D::Error> {
        Ok(match self.0 {
            Value::Bool(value) => bool::deserialize(deserializer)? == value,
            Value::Int(value) => i64::deserialize(deserializer)? == value,
            Value::Uint(value) => u64::deserialize(deserializer)? == value,
            #[expect(clippy::float_cmp, reason = "the value is the one the contract writes")]
            Value::Float(value) => f64::deserialize(deserializer)? == value,
            Value::Str(value) => String::deserialize(deserializer)? == value,
        })
    }
}

/// Whether the text of an error that a type gave as it read a body is serde's own for a field it does not know, one
/// of `fields`: a type with `#[serde(deny_unknown_fields)]`, which a body with fixed fields cannot be read into.
#[cfg(feature = "server")]
pub(crate) fn refused_by(text: &str, fields: &[Field]) -> Option<&'static str> {
    fields
        .iter()
        .find(|field| text.contains(&format!("unknown field `{}`", field.name)))
        .map(|field| field.name)
}

/// The error of a type that refuses a field `name` the contract fixes, which reads the body with it.
#[cfg(feature = "server")]
pub(crate) fn refused_message(name: &str) -> String {
    format!(
        "the type of the body refuses the field `{name}`, which the contract fixes and the body holds: remove its `#[serde(deny_unknown_fields)]`, as the handler's type reads the body with its fixed fields"
    )
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use serde::Serialize;

    use super::*;

    const STREAM: &[Field] = &[Field {
        name: "stream",
        value: Value::Bool(true),
    }];

    const BOTH: &[Field] = &[
        Field {
            name: "stream",
            value: Value::Bool(true),
        },
        Field {
            name: "n",
            value: Value::Int(2),
        },
    ];

    #[derive(Serialize)]
    struct Request {
        model: &'static str,
        stream: bool,
    }

    #[derive(Serialize)]
    struct Wrapper(Request);

    #[derive(Serialize)]
    #[serde(tag = "kind")]
    enum Tagged {
        Message { text: &'static str },
    }

    #[derive(Serialize)]
    struct Flattened {
        model: &'static str,
        #[serde(flatten)]
        extra: BTreeMap<&'static str, u32>,
    }

    fn json<T: Serialize>(value: &T, fields: &'static [Field]) -> serde_json::Result<String> {
        serde_json::to_string(&With::new(value, fields))
    }

    #[test]
    fn the_fields_are_added_to_a_struct_and_replace_its_own() {
        let request = Request {
            model: "m",
            stream: false,
        };

        assert_eq!(
            json(&request, BOTH).unwrap(),
            r#"{"stream":true,"n":2,"model":"m"}"#
        );
        assert_eq!(
            json(&Wrapper(request), STREAM).unwrap(),
            r#"{"stream":true,"model":"m"}"#
        );
    }

    #[test]
    fn the_fields_are_added_to_a_map_a_tagged_enum_and_a_flattened_struct() {
        let map = BTreeMap::from([("stream", 0), ("a", 1)]);
        let tagged = Tagged::Message { text: "hi" };
        let flattened = Flattened {
            model: "m",
            extra: BTreeMap::from([("stream", 3)]),
        };

        assert_eq!(json(&map, STREAM).unwrap(), r#"{"stream":true,"a":1}"#);
        assert_eq!(
            json(&Some(tagged), STREAM).unwrap(),
            r#"{"stream":true,"kind":"Message","text":"hi"}"#
        );
        assert_eq!(
            json(&flattened, STREAM).unwrap(),
            r#"{"stream":true,"model":"m"}"#
        );
    }

    #[test]
    fn a_body_that_is_no_struct_or_map_is_refused() {
        let error = json(&vec![1, 2], STREAM).unwrap_err().to_string();

        assert!(error.contains("this one is a sequence"), "{error}");
        assert!(json(&3, STREAM).is_err());
        assert!(json(&None::<Request>, STREAM).is_err());
    }

    #[test]
    fn a_body_holds_its_fields_once_and_of_their_values() {
        let holds =
            |text: &str, fields| holds(&mut serde_json::Deserializer::from_str(text), fields);

        assert!(holds(r#"{"model":"m","stream":true,"n":2}"#, BOTH));
        assert!(holds(
            r#"{"n":2,"stream":true,"deep":{"stream":false}}"#,
            BOTH
        ));
        assert!(!holds(r#"{"model":"m","stream":true}"#, BOTH));
        assert!(!holds(r#"{"stream":false,"n":2}"#, BOTH));
        assert!(!holds(r#"{"stream":"true","n":2}"#, BOTH));
        assert!(!holds(r#"{"stream":true,"stream":true,"n":2}"#, BOTH));
        assert!(!holds(r#"[true]"#, BOTH));
        assert!(!holds(r#"{"stream":true,"n":2.5}"#, BOTH));
    }

    #[test]
    fn two_routes_are_told_apart_by_a_field_of_one_kind_and_two_values() {
        let field = |name, value| [Field { name, value }];

        assert!(tell_apart(
            &field("stream", Value::Bool(true)),
            &field("stream", Value::Bool(false))
        ));
        assert!(tell_apart(
            &field("n", Value::Int(1)),
            &field("n", Value::Float(1.5))
        ));
        assert!(!tell_apart(
            &field("n", Value::Int(1)),
            &field("n", Value::Float(1.0))
        ));
        assert!(!tell_apart(
            &field("a", Value::Bool(true)),
            &field("b", Value::Bool(false))
        ));
        assert!(!tell_apart(
            &field("a", Value::Bool(true)),
            &field("a", Value::Str("true"))
        ));
        assert!(!tell_apart(
            &field("a", Value::Str("x")),
            &field("a", Value::Str("x"))
        ));
        assert!(tell_apart(
            &field("a", Value::Str("x")),
            &field("a", Value::Str("y"))
        ));
    }
}
