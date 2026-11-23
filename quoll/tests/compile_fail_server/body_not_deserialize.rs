use schemars::JsonSchema;
use serde::Serialize;

// Written by the client, but never read by the server.
#[derive(Serialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
}

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: NewNote);
    }
}

fn main() {}
