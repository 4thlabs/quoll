//! The fixed fields of a body, `#[body("stream" = true)]` on a route: what the generated code names them by, whatever
//! the features, the adapter that adds them to the body the client writes, the check that a body the server reads
//! holds them, and whether two routes of one method and path are told apart by them, at compile time.
//!
//! The adapter and the check go through serde alone, so that any self-describing format of serde's takes them, JSON
//! and forms today: the body is written once, with its fields, and read twice, once for its fields and once into the
//! handler's type, without an intermediate value.

use std::fmt;

use serde::de::{DeserializeSeed, Deserializer, IgnoredAny, MapAccess, Visitor};
use serde::{Deserialize, Serialize, Serializer};

mod write;

pub use write::With;

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

/// Whether the body `deserializer` reads is a struct or a map that holds each of `fields` once, of its value, read
/// as a struct of those fields alone would read them: any other key is skipped. A field missing, given twice or of
/// another value is not held.
///
/// # Errors
///
/// The error of the format when the body does not read as a struct or a map, or a field as the type of its value: a
/// mistake of syntax, a list, a string for a bool.
pub fn holds<'de, D: Deserializer<'de>>(
    deserializer: D,
    fields: &'static [Field],
) -> Result<bool, D::Error> {
    deserializer.deserialize_map(Holds(fields))
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

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use serde::{Deserialize, Serialize};

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
    fn a_format_that_writes_the_length_reads_the_body_back() {
        #[derive(Debug, PartialEq, Deserialize)]
        struct Read {
            model: String,
            stream: bool,
            n: i64,
        }

        let request = Request {
            model: "m",
            stream: false,
        };
        let map = BTreeMap::from([("model", "m"), ("stream", "no")]);
        let expected = Read {
            model: "m".to_owned(),
            stream: true,
            n: 2,
        };

        for written in [
            rmp_serde::to_vec_named(&With::new(&request, BOTH)).unwrap(),
            rmp_serde::to_vec_named(&With::new(&map, BOTH)).unwrap(),
        ] {
            let read: Read = rmp_serde::from_slice(&written).unwrap();

            assert_eq!(read, expected);
        }
    }

    #[test]
    fn a_key_written_as_text_is_left_out_as_its_text() {
        const ONE: &[Field] = &[Field {
            name: "1",
            value: Value::Bool(true),
        }];
        let map = BTreeMap::from([(1, 0), (2, 0)]);

        assert_eq!(json(&map, ONE).unwrap(), r#"{"1":true,"2":0}"#);
    }

    #[test]
    fn a_float_key_is_left_out_as_serde_json_writes_it() {
        /// A map whose keys are floats, which no map of the standard library holds.
        struct Floats;

        impl Serialize for Floats {
            fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                use serde::ser::SerializeMap;

                let mut map = serializer.serialize_map(Some(3))?;
                map.serialize_entry(&1.0_f64, &0)?;
                map.serialize_entry(&1.1_f32, &0)?;
                map.serialize_entry(&2.5_f64, &0)?;
                map.end()
            }
        }

        const FLOATS: &[Field] = &[
            Field {
                name: "1.0",
                value: Value::Bool(true),
            },
            Field {
                name: "1.1",
                value: Value::Bool(false),
            },
        ];

        assert_eq!(
            json(&Floats, FLOATS).unwrap(),
            r#"{"1.0":true,"1.1":false,"2.5":0}"#
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

        assert!(holds(r#"{"model":"m","stream":true,"n":2}"#, BOTH).unwrap());
        assert!(holds(r#"{"n":2,"stream":true,"deep":{"stream":false}}"#, BOTH).unwrap());
        assert!(!holds(r#"{"model":"m","stream":true}"#, BOTH).unwrap());
        assert!(!holds(r#"{"stream":false,"n":2}"#, BOTH).unwrap());
        assert!(!holds(r#"{"stream":true,"stream":true,"n":2}"#, BOTH).unwrap());
    }

    #[test]
    fn a_body_that_does_not_read_is_an_error_not_a_body_without_its_fields() {
        let holds = |text: &str| holds(&mut serde_json::Deserializer::from_str(text), BOTH);

        assert!(holds(r#"{"stream":tru"#).is_err());
        assert!(holds(r#"[true]"#).is_err());
        assert!(holds(r#"{"stream":"true","n":2}"#).is_err());
        assert!(holds(r#"{"stream":true,"n":2.5}"#).is_err());
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
