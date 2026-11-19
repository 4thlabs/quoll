use quoll::rest;
use serde::{Deserialize, Serialize};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> Note;
    }
}

// Without the feature `client`, the contract writes none of these names, which are free for the crate's own.
struct Notes;
struct NotesClient;
struct ApiClient;

impl Api {
    fn client() {}
    fn client_with() {}
}

#[derive(Serialize, Deserialize)]
pub struct Note;

fn main() {
    let _ = (Api, Notes, NotesClient, ApiClient, Api::client(), Api::client_with(), Note);
}
