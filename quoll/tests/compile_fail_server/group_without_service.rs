use quoll::rest::{self, Result};

rest::api! {
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
    async fn get(&self, id: i64) -> Result<String> {
        Ok(id.to_string())
    }
}

fn main() {
    let _ = Api::router(ApiServices { notes: App });
}
