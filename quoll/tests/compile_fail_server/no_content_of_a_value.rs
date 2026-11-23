use quoll::rest::{self, Response, Result, Status};

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct App;

impl NotesServer for App {
    // A 204 has no body, which a route of a value has: it returns `-> Option<String>`.
    async fn get(&self, _id: i64) -> Result<Response<String>> {
        Ok(Response::new(String::new()).with_status(Status::NO_CONTENT))
    }
}

fn main() {}
