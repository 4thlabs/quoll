//! The contract of the notes example: notes kept per user behind a bearer token, a token for a login, and statistics
//! for the operators, behind their key.
//!
//! The crate passes quoll's features on: the server depends on it with `server`, a client with `client`.

use quoll::rest::{self, Created, Events, Form};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// A note, as it is read back.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    /// The note's id, given by the server.
    pub id: i64,
    /// Its title.
    pub title: String,
    /// Its text, where a `#word` tags it.
    pub body: String,
}

/// A note to write: what a note holds, but its id.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    /// Its title.
    pub title: String,
    /// Its text, where a `#word` tags it.
    pub body: String,
}

/// Which notes a search gives: each key that is given narrows it.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    /// Notes whose title or text holds this, whatever the case of its ASCII letters.
    pub contains: Option<String>,
    /// Notes tagged so, by a `#word` of their title or their text, with or without its `#`.
    pub tag: Option<String>,
    /// The page, of 20 notes in the order they were written, from 1, the first when it is missing.
    pub page: Option<u32>,
}

/// A change of a note, as it happens.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum NoteEvent {
    /// The note was written again: as it is now.
    Edited(Note),
    /// The note was removed: the last event of its stream.
    Removed {
        /// The id of the note.
        id: i64,
    },
}

/// A user's name and password, traded for a token.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Login {
    /// The user's name.
    pub user: String,
    /// Their password.
    pub password: String,
}

/// The token a login gives, which the routes of the notes take as a bearer token.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct TokenResponse {
    /// The token, sent as `Authorization: Bearer <token>`.
    pub access_token: String,
}

/// What the server holds, for its operators.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Stats {
    /// How many users can sign in.
    pub users: u64,
    /// How many notes they keep, all users together.
    pub notes: u64,
}

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    /// The key of an operator's scripts.
    pub scheme Key = header "X-Api-Key";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Form<Login>) -> TokenResponse;
    }

    /// The signed-in user's notes.
    #[auth(Bearer)]
    pub group Notes {
        /// Notes matching the filter, a page at a time.
        GET    "/notes"             => search(query filter: NoteFilter) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        /// The changes of a note, as they happen.
        GET    "/notes/{id}/events" => events(id: i64) -> Events<NoteEvent>;
    }

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> Stats;
    }
}
