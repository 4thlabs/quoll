use quoll::rest;
use serde::{Deserialize, Serialize};

// A `Created` of the application's own, which the contract reads as quoll's.
pub struct Created<T>(pub T);

#[derive(Serialize, Deserialize)]
pub struct Note {
    pub id: i64,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: Note) -> Created<Note>;
    }
}

fn main() {}
