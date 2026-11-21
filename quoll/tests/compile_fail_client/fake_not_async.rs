use quoll::rest::{self, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct Fake;

// A fake's method returns a call: an `async move` block, or any future of a `rest::Result`.
impl Notes for Fake {
    fn get(&self, id: i64) -> Result<String> {
        Ok(id.to_string())
    }
}

fn main() {}
