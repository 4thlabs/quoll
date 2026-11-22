use quoll::rest;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[rest::query]
#[derive(Serialize, Deserialize, JsonSchema)]
pub enum Filter {
    Open,
}

fn main() {}
