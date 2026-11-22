use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[rest::query]
#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Filter {
    #[join(",")]
    #[schemars(with = "Vec<String>")]
    pub labels: Vec<String>,
}

fn main() {}
