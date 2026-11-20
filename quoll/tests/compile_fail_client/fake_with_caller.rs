use quoll::rest::{self, Result};

rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct User;
struct Fake;

// The client's trait has no caller: its middleware sends the credential.
impl Notes for Fake {
    async fn get(&self, _user: &User, id: i64) -> Result<String> {
        Ok(id.to_string())
    }
}

fn main() {}
