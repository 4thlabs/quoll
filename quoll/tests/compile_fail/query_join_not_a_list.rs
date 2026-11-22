use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[rest::query]
#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Filter {
    #[join(",")]
    pub labels: Option<String>,
}

fn main() {}
