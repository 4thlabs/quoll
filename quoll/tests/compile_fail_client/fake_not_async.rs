use quoll::{Arg, Result};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct Fake;

// A fake's method returns a call: an `async move` block, or any future of a `quoll::Result`.
impl Notes for Fake {
    fn get(&self, id: impl Arg<i64>) -> Result<String> {
        Ok(id.into_value().to_string())
    }
}

fn main() {}
