use quoll::rest;
use serde::Deserialize;

// Read by the server, but never written by the client.
#[derive(Deserialize)]
pub struct NewNote {
    pub title: String,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: NewNote);
    }
}

fn main() {}
