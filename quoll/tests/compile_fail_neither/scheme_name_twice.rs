use http::request::Parts;
use http::HeaderName;
use quoll::auth::{Outgoing, Unsendable};
use quoll::{Result, ClientScheme, Scheme, ServerScheme, openapi::SecurityScheme};

/// A scheme of the application's own that takes the name of the crate's `Bearer`.
pub struct Token;

impl Scheme for Token {
    const NAME: &'static str = "Bearer";
}

impl ServerScheme for Token {
    type Credential = String;

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Token")
    }
}

impl ClientScheme for Token {
    type Secret = String;

    fn write(secret: &String, request: &mut Outgoing<'_>) -> std::result::Result<(), Unsendable> {
        request.header(HeaderName::from_static("x-token"), secret)
    }
}

quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer | Token)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
