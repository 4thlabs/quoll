use quoll::rest::{self, Response, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> String;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<String>> {
        Ok(Response::new(id.to_string()))
    }
}

fn main() {}
