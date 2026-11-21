use quoll::rest::{self, Response, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

// The handler takes an argument of another type than the route's.
impl NotesServer for App {
    async fn get(&self, id: String) -> Result<Response<String>> {
        Ok(Response::new(id))
    }
}

fn main() {}
