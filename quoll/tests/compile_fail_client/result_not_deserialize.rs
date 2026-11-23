use quoll::JsonLines;
use schemars::JsonSchema;
use serde::Serialize;

// Written by the server, but never read by the client.
#[derive(Serialize, JsonSchema)]
pub struct Note {
    pub id: i64,
}

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> Note;
        GET "/export"     => export() -> JsonLines<Note>;
    }
}

fn main() {}
