use quoll::{Created, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
}

#[derive(Clone, Copy, Serialize, Deserialize, JsonSchema)]
pub enum Shelf {
    Kitchen,
}

quoll::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check();
    }

    /// The notes.
    pub group Notes {
        /// Notes, a page at a time.
        GET    "/notes?contains={contains}&page={page}&tag={tags}"
               => list(contains: Option<String>, page: Option<u32>, tags: Vec<String>) -> Vec<Note>;
        GET    "/shelves/{shelf}/notes/{id}" => get(id: i64, shelf: Shelf) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        OPTIONS "/notes"            => options();
    }

    pub group AdminTools {
        PATCH "/admin/{service}?query={query}"
              => r#match(service: String, query: Option<String>, request: String) -> String;
    }

    pub group Type {
        GET "/types" => list() -> Vec<String>;
    }
}

// The client's methods, as a caller names them: a `Created<T>` gives the `T`, a route with no result `()`.
async fn calls(api: &ApiClient) -> Result<()> {
    let (): () = api.health().check().await?;
    let _: Vec<Note> = api.notes().list(None, Some(1), Vec::new()).await?;
    let _: Note = api.notes().get(1, Shelf::Kitchen).await?;
    let _: Note = api.notes().create(NewNote { title: "Milk".into() }).await?;
    let _: Note = api.notes().update(1, NewNote { title: "Bread".into() }).await?;
    let (): () = api.notes().remove(1).await?;
    let (): () = api.notes().options().await?;
    let _: String = api.admin_tools().r#match("a", None, "b").await?;
    let _: Vec<String> = api.r#type().list().await?;

    Ok(())
}

fn is_send<T: Send>(_: T) {}

fn main() -> Result<()> {
    let api: ApiClient = Api::client("http://localhost:8080")?;
    let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new()).build();
    let _with: ApiClient = Api::client_with(http, "http://localhost:8080")?;
    let _notes: NotesClient = api.notes();

    is_send(calls(&api));
    Ok(())
}
