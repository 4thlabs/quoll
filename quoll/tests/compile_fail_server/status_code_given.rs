use http::StatusCode;
use quoll::rest::{self, Response, Result};

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Notes {
        PUT "/notes/{id}" => put(id: i64);
    }
}

struct App;

impl NotesServer for App {
    // A status is one of `rest::Status`'s, or `Status::try_from(code)`.
    async fn put(&self, _id: i64) -> Result<Response<()>> {
        Ok(Response::new(()).with_status(StatusCode::CREATED))
    }
}

fn main() {}
