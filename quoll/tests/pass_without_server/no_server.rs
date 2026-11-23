use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> Note;
    }
}

// Without the feature `server`, the contract writes none of these names, which are free for the crate's own.
trait NotesServer {}
struct ApiServices;

impl Api {
    fn router() {}
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note;

fn main() {
    let _ = (Api, ApiServices, Api::router(), Note);
}
