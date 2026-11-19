use quoll::rest::{self, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, id: String) -> Result<String> {
        Ok(id)
    }
}

fn main() {}
