use quoll::rest::{self, Created, Error, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
}

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check();
    }

    /// The notes.
    pub group Notes {
        /// Notes, a page at a time.
        GET    "/notes?contains={contains}&page={page}"
               => list(contains: Option<String>, page: Option<u32>) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        OPTIONS "/notes"            => options();
    }

    pub group AdminTools {
        PATCH "/admin/{service}?query={query}"
              => r#match(service: String, query: Option<String>, request: String) -> String;
    }
}

#[derive(Clone)]
struct App;

impl HealthServer for App {
    async fn check(&self) -> Result<()> {
        Ok(())
    }
}

impl NotesServer for App {
    async fn list(&self, _contains: Option<String>, _page: Option<u32>) -> Result<Vec<Note>> {
        Ok(Vec::new())
    }

    async fn get(&self, _id: i64) -> Result<Note> {
        Err(Error::not_found("no such note"))
    }

    async fn create(&self, note: NewNote) -> Result<Created<Note>> {
        Ok(Created::at("/api/notes/1", Note { id: 1, title: note.title }))
    }

    async fn update(&self, id: i64, note: NewNote) -> Result<Note> {
        Ok(Note { id, title: note.title })
    }

    async fn remove(&self, _id: i64) -> Result<()> {
        Ok(())
    }

    async fn options(&self) -> Result<()> {
        Ok(())
    }
}

// A service holds its own state, and its arguments may take the names the generated code gives its own locals.
struct Admin {
    name: String,
}

impl AdminToolsServer for Admin {
    async fn r#match(&self, service: String, query: Option<String>, request: String) -> Result<String> {
        Ok(format!("{} {service} {query:?} {request}", self.name))
    }
}

fn main() {
    let _router: axum::Router = Api::router(ApiServices {
        health: App,
        notes: App,
        admin_tools: Admin { name: "admin".into() },
    });
}
