use quoll::rest::{self, Response, Result};

rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, _user: &String, id: i64) -> Result<Response<String>> {
        Ok(Response::new(id.to_string()))
    }
}

fn main() {
    let _ = Api::router(ApiServices { notes: App });
}
