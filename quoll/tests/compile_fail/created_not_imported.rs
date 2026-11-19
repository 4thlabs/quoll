use quoll::rest;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
pub struct Note {
    pub id: i64,
}

// `Created` is the contract's own tokens: without its import, it is not found.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: Note) -> Created<Note>;
    }
}

fn main() {}
