use quoll::rest;
use serde::Serialize;

// Written by the client, but never read by the server.
#[derive(Serialize)]
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
