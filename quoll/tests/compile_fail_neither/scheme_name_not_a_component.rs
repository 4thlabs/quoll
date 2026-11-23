use http::request::Parts;
use quoll::{Result, Scheme, openapi::SecurityScheme};

/// A scheme of the application's own whose name cannot be a component's: a space.
pub struct Signature;

impl Scheme for Signature {
    type Credential = String;

    const NAME: &'static str = "Request Signature";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

quoll::api! {
    pub api Api = "/api";

    #[auth(Signature)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
