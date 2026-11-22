//! A list written as the one value of a query key, `labels=bug,docs`, on the field of a query struct.
//!
//! A route's argument says it with `#[join(sep)]`, which the macro reads. A field of a query struct, which the macro
//! does not see, names one of these types in serde's `with`, with the list's `T` for an exact schema:
//!
//! ```
//! use quoll::rest;
//! use schemars::JsonSchema;
//! use serde::{Deserialize, Serialize};
//!
//! #[derive(Default, Serialize, Deserialize, JsonSchema)]
//! #[serde(default)]
//! pub struct IssueFilter {
//!     /// `ids=1,2`, whose items the document says are integers.
//!     #[serde(with = "rest::join::Comma::<u32>")]
//!     pub ids: Vec<u32>,
//!     /// `labels=bug+docs`, whose items the document leaves open.
//!     #[serde(with = "rest::join::Space")]
//!     pub labels: Vec<String>,
//! }
//!
//! rest::api! {
//!     pub api Api = "/api";
//!
//!     pub group Issues {
//!         GET "/issues" => search(query filter: IssueFilter) -> Vec<String>;
//!     }
//! }
//! ```
//!
//! serde calls their `serialize` and `deserialize`, and infers the `T` from the field. schemars takes the type as it is
//! written, and never sees the field: without a `T`, the `T` is [`Any`], and the schema of the items says nothing.
//!
//! A list is written as one value, its values' text joined by the separator, which serde_html_form escapes,
//! `ids=1%2C2`, or as nothing when it is empty, which leaves its key out. A value is read from text, as a path
//! parameter is, and a value that holds the separator, or a list of one empty value, which would read back otherwise,
//! cannot be written. The value is decoded before it is split, so that `ids=1,2` and `ids=1%2C2` read alike; an empty
//! value is an empty list, and so is a missing key for a field with `#[serde(default)]`.
//!
//! In JSON too a joined field is its text, or `null` for an empty list, as its schema says. Its schema carries a mark
//! that the OpenAPI document turns into the parameter's style, `explode: false`, with the list's own schema, as it
//! unfolds a query struct into its keys.

use std::borrow::Cow;
use std::marker::PhantomData;

use schemars::{JsonSchema, Schema, SchemaGenerator, json_schema};
use serde::de::{DeserializeOwned, Deserializer};
use serde::ser::{Serialize, Serializer};

use crate::__private::join::{self, Join};

/// The `T` of a joined list written without one: a value of any schema. No value has this type; serde infers the
/// field's own `T` in its place.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Any {}

impl JsonSchema for Any {
    fn inline_schema() -> bool {
        true
    }

    fn schema_name() -> Cow<'static, str> {
        "Any".into()
    }

    fn schema_id() -> Cow<'static, str> {
        "quoll::rest::join::Any".into()
    }

    fn json_schema(_: &mut SchemaGenerator) -> Schema {
        json_schema!({})
    }
}

/// Writes a type named in serde's `with` for each separator.
macro_rules! joined {
    ($($(#[$docs:meta])* $name:ident => $join:ident;)*) => {
        $(
            $(#[$docs])*
            #[derive(Debug)]
            pub struct $name<T = Any>(PhantomData<fn() -> T>);

            impl<T: Serialize> $name<T> {
                /// Writes the list as one value, or as nothing when it is empty.
                ///
                /// # Errors
                ///
                /// When a value is not one piece of text, holds the separator, or is the list's one value and empty.
                pub fn serialize<S: Serializer>(values: &[T], serializer: S) -> Result<S::Ok, S::Error> {
                    join::serialize(Join::$join, values, serializer)
                }
            }

            impl<T: DeserializeOwned> $name<T> {
                /// Reads the list back from its one value.
                ///
                /// # Errors
                ///
                /// When the value is not a string, or one of its pieces is not a `T`.
                pub fn deserialize<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Vec<T>, D::Error> {
                    join::deserialize(Join::$join, deserializer)
                }
            }

            impl<T: JsonSchema> JsonSchema for $name<T> {
                fn inline_schema() -> bool {
                    true
                }

                fn schema_name() -> Cow<'static, str> {
                    Vec::<T>::schema_name()
                }

                fn schema_id() -> Cow<'static, str> {
                    format!(concat!("quoll::rest::join::", stringify!($name), "<{}>"), T::schema_id()).into()
                }

                fn json_schema(generator: &mut SchemaGenerator) -> Schema {
                    join::json_schema::<T>(Join::$join, generator)
                }
            }
        )*
    };
}

joined! {
    /// A list joined with `,`, `ids=1,2`: `style: form, explode: false` in OpenAPI.
    Comma => Comma;
    /// A list joined with a space, `ids=1+2`: `style: spaceDelimited`.
    Space => Space;
    /// A list joined with `|`, `ids=1|2`: `style: pipeDelimited`.
    Pipe => Pipe;
}
