use quoll::rest;
use serde::{Deserialize, Serialize};

// The OpenAPI document describes every type the contract names, which has a schema.
#[derive(Serialize, Deserialize)]
pub struct Note {
    pub title: String,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> Note;
    }
}

fn main() {}
