use quoll::{Call, Created, Response, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
}

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{id}" => get(id: i64) -> Note;
        POST "/notes"      => create(note: Note) -> Created<Note>;
    }
}

// One type is both a group's service and its client's stand-in: the two traits sit side by side.
struct Local;

impl NotesServer for Local {
    async fn get(&self, id: i64) -> Result<Response<Note>> {
        Ok(Response::new(Note { id }))
    }

    async fn create(&self, note: Note) -> Result<Response<Note>> {
        Ok(Response::new(note))
    }
}

impl Notes for Local {
    fn get(&self, id: i64) -> impl Call<Note> {
        async move {
            Ok(Note { id })
        }
    }

    fn create(&self, note: Note) -> impl Call<Note> {
        async move {
            Ok(note)
        }
    }
}

// A server whose handler calls another api through the client.
struct Forward(ApiClient);

impl NotesServer for Forward {
    async fn get(&self, id: i64) -> Result<Response<Note>> {
        self.0.notes().get(id).await.map(Response::new)
    }

    async fn create(&self, note: Note) -> Result<Response<Note>> {
        self.0.notes().create(note).await.map(Response::new)
    }
}

fn main() -> Result<()> {
    let _local: axum::Router = Api::router(ApiServices { notes: Local });
    let _forward: axum::Router = Api::router(ApiServices {
        notes: Forward(Api::client("http://localhost:8081")?),
    });

    Ok(())
}
