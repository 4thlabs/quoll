use quoll::rest::{self, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> String;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

struct Fake;

impl Notes for Fake {
    async fn get(&self, _id: i64) -> Result<String> {
        Ok(String::new())
    }
}

fn main() {}
