use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

mod words {
    pub fn serialize<S: serde::Serializer>(_: &[String], _: S) -> Result<S::Ok, S::Error> {
        unimplemented!()
    }
    pub fn deserialize<'de, D: serde::Deserializer<'de>>(_: D) -> Result<Vec<String>, D::Error> {
        unimplemented!()
    }
}

#[rest::query]
#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Filter {
    #[join(",")]
    #[serde(default, with = "words")]
    // What schemars asks beside a `with` of serde's that names a module.
    #[schemars(with = "Vec<String>")]
    pub labels: Vec<String>,
}

fn main() {}
