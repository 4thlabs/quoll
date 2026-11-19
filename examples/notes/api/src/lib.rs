//! The contract of the notes example: notes kept per user, behind a bearer token.
//!
//! The crate passes quoll's features on: the server depends on it with `server`, a client with `client`.

use quoll::rest::{self, Created};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// A note, as it is read back.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    /// The note's id, given by the server.
    pub id: i64,
    /// Its title.
    pub title: String,
    /// Its text.
    pub body: String,
}

/// A note to write: what a note holds, but its id.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    /// Its title.
    pub title: String,
    /// Its text.
    pub body: String,
}

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    /// The signed-in user's notes.
    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}
