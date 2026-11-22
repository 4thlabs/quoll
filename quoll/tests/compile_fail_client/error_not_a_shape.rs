use quoll::rest;
use serde::Deserialize;

// Read from JSON, but with no `Display` to give the error's message.
#[derive(Debug, Deserialize)]
pub struct HubError {
    pub message: String,
}

rest::api! {
    #[openapi(none)]
    #[error(HubError)]
    pub api Hub = "";

    pub group Repos {
        GET "/repos" => list();
    }
}

fn main() {}
