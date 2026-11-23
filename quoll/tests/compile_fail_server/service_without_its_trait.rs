use quoll::{Response, Result};

quoll::api! {
    pub api Api = "/api";

    pub group Auth {
        POST "/token" => token(login: String) -> String;
    }

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<String>> {
        Ok(Response::new(id.to_string()))
    }
}

fn main() {
    let _ = Api::router(ApiServices { auth: App, notes: App });
}
