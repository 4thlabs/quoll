use quoll::rest::{self, Accepted, Result};

rest::api! {
    pub api Api = "/api";

    pub group Exports {
        POST "/exports" => export(name: String) -> Accepted<String>;
    }
}

// On the client, an `Accepted<T>` route gives the `T`: the 202 is the server's answer only.
struct Fake;

impl Exports for Fake {
    async fn export(&self, name: String) -> Result<Accepted<String>> {
        Ok(Accepted::new(name))
    }
}

fn main() {}
