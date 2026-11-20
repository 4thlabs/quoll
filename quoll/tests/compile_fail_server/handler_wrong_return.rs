use quoll::rest::{self, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

// The handler returns another type than the route's result.
impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<i64> {
        Ok(id)
    }
}

fn main() {}
