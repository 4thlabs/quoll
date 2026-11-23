use quoll::{Bytes, Created, Form, Text};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note;

#[derive(Default, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    pub contains: Option<String>,
    #[serde(default)]
    pub tags: Vec<String>,
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Login {
    pub user: String,
}

// The types the contract names by their name are the user's imports, used whatever the features of quoll.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes" => search(query filter: NoteFilter, header("Last-Event-ID") last: Option<String>) -> Vec<Note>;
        POST "/token" => token(login: Form<Login>) -> Created<Note>;
        PUT  "/notes/{id}/text" => write(id: i64, header("If-Match") version: u64, text: Text);
        PUT  "/notes/{id}/file" => upload(id: i64, file: Bytes);
        POST "/notes/{id}/copy" => copy(id: i64, file: quoll::Bytes);
    }
}

fn main() {
    let _api = Api;
}
