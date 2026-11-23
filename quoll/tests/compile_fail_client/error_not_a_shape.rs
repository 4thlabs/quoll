use serde::Deserialize;

// Read from JSON, but with no `Display` to give the error's message.
#[derive(Debug, Deserialize)]
pub struct HubError {
    pub message: String,
}

quoll::api! {
    #[openapi(none)]
    #[error(HubError)]
    pub api Hub = "";

    // One error, on the attribute, whatever the routes that take it.
    pub group Repos {
        GET "/repos" => list();
        GET "/repos/{name}" => get(name: String) -> String;
    }
}

fn main() {}
