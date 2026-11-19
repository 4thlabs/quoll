use quoll::rest::{self, Created, Result};

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(title: String) -> Created<String>;
    }
}

// On the client, a `Created<T>` route gives the `T`: the 201 is the server's answer only.
struct Fake;

impl Notes for Fake {
    async fn create(&self, title: String) -> Result<Created<String>> {
        Ok(Created::new(title))
    }
}

fn main() {}
