use quoll::rest::{self, Events};
use schemars::JsonSchema;
use serde::Deserialize;

// Read by the client, but never written by the server.
#[derive(Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}"        => get(id: i64) -> Note;
        GET "/notes/{id}/events" => events(id: i64) -> Events<Note>;
    }
}

fn main() {}
