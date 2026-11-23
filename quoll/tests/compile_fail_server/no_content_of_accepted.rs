use quoll::{Accepted, Response, Result, Status};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub title: String,
}

quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Notes {
        PUT "/notes" => write(title: String) -> Accepted<Note>;
    }
}

struct App;

impl NotesServer for App {
    // A 204 has no body, which an `Accepted<Note>` carries.
    async fn write(&self, title: String) -> Result<Response<Note>> {
        Ok(Response::new(Note { title }).with_status(Status::NO_CONTENT))
    }
}

fn main() {}
