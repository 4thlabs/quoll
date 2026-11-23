use quoll::{Response, Result};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

// A handler is an `async fn`, or a `fn` returning a future.
impl NotesServer for App {
    fn get(&self, id: i64) -> Result<Response<String>> {
        Ok(Response::new(id.to_string()))
    }
}

fn main() {}
