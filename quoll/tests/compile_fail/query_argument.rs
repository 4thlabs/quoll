use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[rest::query(strict)]
#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Filter {
    #[join(",")]
    pub labels: Vec<String>,
}

fn main() {}
