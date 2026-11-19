use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{self, Result, Scheme};

/// A scheme of the application's own that takes the name of the crate's `Bearer`.
pub struct Token;

impl Scheme for Token {
    type Credential = String;

    const NAME: &'static str = "Bearer";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Token")
    }
}

rest::api! {
    pub api Api = "/api";

    #[auth(Bearer | Token)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
