//! The adapter that adds a route's fixed fields to the body the client writes, through the serializer of its format:
//! at the top of the struct or the map the body serializes as, a field of the body's own of the same name left out,
//! and the length the format is given exact, as MessagePack and CBOR write it.

use std::fmt;

use serde::Serialize;
use serde::ser::{self, Impossible, SerializeMap, SerializeStruct, Serializer};

use super::Field;

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
        // A first pass counts the fields of `value` that the fixed ones replace, for the length of the second. A value
        // it cannot count is no struct or map, which the second refuses with its message.
        let replaced = self.value.serialize(Counter(self.fields)).unwrap_or(0);

        self.value.serialize(Adder {
            inner: serializer,
            fields: self.fields,
            replaced,
        })
    }
}

/// Whether `name` is one of `fields`.
fn is_fixed(fields: &[Field], name: &str) -> bool {
    fields.iter().any(|field| field.name == name)
}

/// The serializer of [`With`]: the format's own, with the fields added at the top of a struct or a map, which hold
/// `replaced` fields of their own of the same names.
struct Adder<S> {
    inner: S,
    fields: &'static [Field],
    replaced: usize,
}

impl<S> Adder<S> {
    /// The length of the struct or the map, `len` of its own, once the fields are added and the ones they replace
    /// left out.
    fn len(&self, len: usize) -> usize {
        len.saturating_sub(self.replaced) + self.fields.len()
    }
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
        let len = len.map(|len| self.len(len));
        let mut inner = self.inner.serialize_map(len)?;

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
        let len = self.len(len);
        let mut inner = self.inner.serialize_struct(name, len)?;

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

/// A struct the fields are added to, which leaves out a field of its own of the same name.
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
        self.skipping = is_fixed_key(self.fields, key);
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
        if is_fixed_key(self.fields, key) {
            return Ok(());
        }

        self.inner.serialize_entry(key, value)
    }

    fn end(self) -> Result<S::Ok, S::Error> {
        self.inner.end()
    }
}

/// The error of the passes that only look at a value, [`Counter`] and [`KeyName`]: one they do not read.
#[derive(Debug)]
struct NotRead;

impl fmt::Display for NotRead {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("not read")
    }
}

impl std::error::Error for NotRead {}

impl ser::Error for NotRead {
    fn custom<T: fmt::Display>(_msg: T) -> Self {
        Self
    }
}

/// The methods of a pass that looks at a value, for what it does not read.
macro_rules! not_read {
    ($ok:ty; $($method:ident($($arg:ty),*);)*) => {
        $(
            fn $method(self, $(_: $arg),*) -> Result<$ok, NotRead> {
                Err(NotRead)
            }
        )*
    };
}

/// The compound methods of a pass that looks at a value, which it does not read, but a struct and a map.
macro_rules! not_read_compounds {
    () => {
        fn serialize_seq(self, _len: Option<usize>) -> Result<Self::SerializeSeq, NotRead> {
            Err(NotRead)
        }

        fn serialize_tuple(self, _len: usize) -> Result<Self::SerializeTuple, NotRead> {
            Err(NotRead)
        }

        fn serialize_tuple_struct(
            self,
            _name: &'static str,
            _len: usize,
        ) -> Result<Self::SerializeTupleStruct, NotRead> {
            Err(NotRead)
        }

        fn serialize_tuple_variant(
            self,
            _name: &'static str,
            _index: u32,
            _variant: &'static str,
            _len: usize,
        ) -> Result<Self::SerializeTupleVariant, NotRead> {
            Err(NotRead)
        }

        fn serialize_struct_variant(
            self,
            _name: &'static str,
            _index: u32,
            _variant: &'static str,
            _len: usize,
        ) -> Result<Self::SerializeStructVariant, NotRead> {
            Err(NotRead)
        }
    };
}

/// A serializer that counts the fields at the top of a struct or a map, or of what an `Option` or a newtype holds,
/// whose names are the fixed ones: what [`Adder`] leaves out. It serializes no value.
struct Counter(&'static [Field]);

impl Serializer for Counter {
    type Ok = usize;
    type Error = NotRead;
    type SerializeSeq = Impossible<usize, NotRead>;
    type SerializeTuple = Impossible<usize, NotRead>;
    type SerializeTupleStruct = Impossible<usize, NotRead>;
    type SerializeTupleVariant = Impossible<usize, NotRead>;
    type SerializeMap = CountMap;
    type SerializeStruct = CountStruct;
    type SerializeStructVariant = Impossible<usize, NotRead>;

    not_read! {
        usize;
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
        serialize_char(char);
        serialize_str(&str);
        serialize_bytes(&[u8]);
        serialize_none();
        serialize_unit();
        serialize_unit_struct(&'static str);
        serialize_unit_variant(&'static str, u32, &'static str);
    }

    not_read_compounds!();

    fn serialize_some<T: Serialize + ?Sized>(self, value: &T) -> Result<usize, NotRead> {
        value.serialize(self)
    }

    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        value: &T,
    ) -> Result<usize, NotRead> {
        value.serialize(self)
    }

    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _value: &T,
    ) -> Result<usize, NotRead> {
        Err(NotRead)
    }

    fn serialize_map(self, _len: Option<usize>) -> Result<CountMap, NotRead> {
        Ok(CountMap {
            fields: self.0,
            count: 0,
        })
    }

    fn serialize_struct(self, _name: &'static str, _len: usize) -> Result<CountStruct, NotRead> {
        Ok(CountStruct {
            fields: self.0,
            count: 0,
        })
    }
}

/// The fields of a struct [`Counter`] counts.
struct CountStruct {
    fields: &'static [Field],
    count: usize,
}

impl SerializeStruct for CountStruct {
    type Ok = usize;
    type Error = NotRead;

    fn serialize_field<T: Serialize + ?Sized>(
        &mut self,
        key: &'static str,
        _value: &T,
    ) -> Result<(), NotRead> {
        if is_fixed(self.fields, key) {
            self.count += 1;
        }

        Ok(())
    }

    fn end(self) -> Result<usize, NotRead> {
        Ok(self.count)
    }
}

/// The entries of a map [`Counter`] counts.
struct CountMap {
    fields: &'static [Field],
    count: usize,
}

impl SerializeMap for CountMap {
    type Ok = usize;
    type Error = NotRead;

    fn serialize_key<T: Serialize + ?Sized>(&mut self, key: &T) -> Result<(), NotRead> {
        if is_fixed_key(self.fields, key) {
            self.count += 1;
        }

        Ok(())
    }

    fn serialize_value<T: Serialize + ?Sized>(&mut self, _value: &T) -> Result<(), NotRead> {
        Ok(())
    }

    fn end(self) -> Result<usize, NotRead> {
        Ok(self.count)
    }
}

/// Whether a map's `key` is the name of one of `fields`, as the format writes it as text.
fn is_fixed_key<K: Serialize + ?Sized>(fields: &'static [Field], key: &K) -> bool {
    key.serialize(KeyName(fields)).unwrap_or(false)
}

/// A serializer that tells whether a map's key is the name of one of the fields, as serde_json writes a key: a
/// string, a character, a unit variant, an integer or a bool, as text; any other key is not.
struct KeyName(&'static [Field]);

/// The methods of [`KeyName`] for a key written as its text, an integer or a bool.
macro_rules! as_text {
    ($($method:ident($arg:ty);)*) => {
        $(
            fn $method(self, value: $arg) -> Result<bool, NotRead> {
                Ok(is_fixed(self.0, &value.to_string()))
            }
        )*
    };
}

impl Serializer for KeyName {
    type Ok = bool;
    type Error = NotRead;
    type SerializeSeq = Impossible<bool, NotRead>;
    type SerializeTuple = Impossible<bool, NotRead>;
    type SerializeTupleStruct = Impossible<bool, NotRead>;
    type SerializeTupleVariant = Impossible<bool, NotRead>;
    type SerializeMap = Impossible<bool, NotRead>;
    type SerializeStruct = Impossible<bool, NotRead>;
    type SerializeStructVariant = Impossible<bool, NotRead>;

    as_text! {
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
        serialize_char(char);
    }

    not_read! {
        bool;
        serialize_f32(f32);
        serialize_f64(f64);
        serialize_bytes(&[u8]);
        serialize_none();
        serialize_unit();
        serialize_unit_struct(&'static str);
    }

    not_read_compounds!();

    fn serialize_str(self, value: &str) -> Result<bool, NotRead> {
        Ok(is_fixed(self.0, value))
    }

    fn serialize_unit_variant(
        self,
        _name: &'static str,
        _index: u32,
        variant: &'static str,
    ) -> Result<bool, NotRead> {
        Ok(is_fixed(self.0, variant))
    }

    fn serialize_some<T: Serialize + ?Sized>(self, value: &T) -> Result<bool, NotRead> {
        value.serialize(self)
    }

    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        value: &T,
    ) -> Result<bool, NotRead> {
        value.serialize(self)
    }

    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        _value: &T,
    ) -> Result<bool, NotRead> {
        Err(NotRead)
    }

    fn serialize_map(self, _len: Option<usize>) -> Result<Self::SerializeMap, NotRead> {
        Err(NotRead)
    }

    fn serialize_struct(
        self,
        _name: &'static str,
        _len: usize,
    ) -> Result<Self::SerializeStruct, NotRead> {
        Err(NotRead)
    }
}
