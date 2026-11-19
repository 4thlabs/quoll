use quoll::rest::{self, Created, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{id}" => get(id: i64) -> Note;
        POST "/notes"      => create(note: Note) -> Created<Note>;
    }
}

// One type is both a group's service and its client's stand-in: the two traits sit side by side.
struct Local;

impl NotesServer for Local {
    async fn get(&self, id: i64) -> Result<Note> {
        Ok(Note { id })
    }

    async fn create(&self, note: Note) -> Result<Created<Note>> {
        Ok(Created::new(note))
    }
}

impl Notes for Local {
    async fn get(&self, id: i64) -> Result<Note> {
        Ok(Note { id })
    }

    async fn create(&self, note: Note) -> Result<Note> {
        Ok(note)
    }
}

// A server whose handler calls another api through the client.
struct Forward(ApiClient);

impl NotesServer for Forward {
    async fn get(&self, id: i64) -> Result<Note> {
        self.0.notes().get(id).await
    }

    async fn create(&self, note: Note) -> Result<Created<Note>> {
        self.0.notes().create(note).await.map(Created::new)
    }
}

fn main() -> Result<()> {
    let _local: axum::Router = Api::router(ApiServices { notes: Local });
    let _forward: axum::Router = Api::router(ApiServices {
        notes: Forward(Api::client("http://localhost:8081")?),
    });

    Ok(())
}
