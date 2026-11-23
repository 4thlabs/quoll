use schemars::JsonSchema;

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> Note;
    }
}

// With neither feature, the contract writes its type alone: the names of both sides are free for the crate's own.
struct Notes;
struct NotesServer;
struct NotesClient;
struct ApiServices;
struct ApiClient;

impl Api {
    fn router() {}
    fn client() {}
    fn client_with() {}
}

#[derive(JsonSchema)]
struct Note;

fn main() {
    let _ = (Notes, NotesServer, NotesClient, ApiServices, ApiClient, Note);
    let _ = (Api::router(), Api::client(), Api::client_with());
}
