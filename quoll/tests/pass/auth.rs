use quoll::Created;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
}

// Whatever the features, a contract with authentication compiles: each side writes what it needs of it.
quoll::api! {
    pub api Api = "/api";

    pub group Auth {
        POST "/token" => token(login: String) -> String;
        /// Revokes the token it is sent with.
        #[auth(Bearer)]
        DELETE "/token" => revoke();
    }

    /// The signed-in user's notes.
    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: Note) -> Created<Note>;
        #[auth(none)]
        GET    "/public"     => public() -> Vec<Note>;
    }

    #[auth(none)]
    pub group Health {
        HEAD "/health" => check();
    }
}

fn main() {}
