use quoll::rest::{self, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct Fake;

// A fake's method is an `async fn`, or a `fn` returning a future.
impl Notes for Fake {
    fn get(&self, id: i64) -> Result<String> {
        Ok(id.to_string())
    }
}

fn main() {}
