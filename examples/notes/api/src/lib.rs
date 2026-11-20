use std::fmt;

use quoll::rest::{self, Created, Events, Form};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
    pub body: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
    pub body: String,
}

pub const CONTAINS_LIMIT: usize = 1000;

/// Each key given, and not empty, narrows the search.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    /// Notes whose title or text holds this, whatever the case of its ASCII letters: 1000 characters at most.
    #[schemars(length(max = CONTAINS_LIMIT))]
    pub contains: Option<String>,
    /// Notes tagged so, by a `#word` of their title or their text: `#milk` or `milk,` is `milk`.
    pub tag: Option<String>,
    /// The page, of 20 notes in the order they were written, from 1, the first when it is missing.
    pub page: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum NoteEvent {
    Edited(Note),
    /// The last event of the note's stream.
    Removed {
        id: i64,
    },
}

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Login {
    pub user: String,
    pub password: String,
}

/// Never shows the password.
impl fmt::Debug for Login {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Login")
            .field("user", &self.user)
            .finish_non_exhaustive()
    }
}

#[derive(Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct TokenResponse {
    /// Sent as `Authorization: Bearer <token>` to the routes of the notes.
    pub access_token: String,
}

/// Never shows the token.
impl fmt::Debug for TokenResponse {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TokenResponse").finish_non_exhaustive()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Stats {
    pub users: u64,
    pub notes: u64,
}

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Form<Login>) -> TokenResponse;
    }

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes"             => search(query filter: NoteFilter) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        GET    "/notes/{id}/events" => events(id: i64) -> Events<NoteEvent>;
    }

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> Stats;
    }
}
