use quoll::rest::{self, Created};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Login) -> TokenResponse;
    }

    /// The signed-in user's notes.
    pub group Notes {
        GET    "/notes?contains={contains}&page={page}"
               => list(contains: Option<String>, page: Option<u32>) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
    }
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Login;
#[derive(Serialize, Deserialize, JsonSchema)]
pub struct TokenResponse;
#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note;
#[derive(Serialize, Deserialize, JsonSchema)]
pub struct NewNote;

fn main() {
    let _api = Api;
}
