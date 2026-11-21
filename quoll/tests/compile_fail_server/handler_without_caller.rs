use quoll::rest::{self, Authenticate, Bearer, Response, Result};

rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct User;
struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, _token: String) -> Result<User> {
        Ok(User)
    }
}

// The handler of a route that takes a scheme receives its caller first, `user: &User`.
impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<String>> {
        Ok(Response::new(id.to_string()))
    }
}

fn main() {}
