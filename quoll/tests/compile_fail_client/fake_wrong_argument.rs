use quoll::rest::{self, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct Fake;

// A fake's method takes the route's arguments, of their types.
impl Notes for Fake {
    async fn get(&self, id: String) -> Result<String> {
        Ok(id)
    }
}

fn main() {}
