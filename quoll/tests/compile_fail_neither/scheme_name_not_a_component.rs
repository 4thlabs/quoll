use http::request::Parts;
use http::HeaderName;
use quoll::auth::{Outgoing, Unsendable};
use quoll::{Result, ClientScheme, Scheme, ServerScheme, openapi::SecurityScheme};

/// A scheme of the application's own whose name cannot be a component's: a space.
pub struct Signature;

impl Scheme for Signature {
    const NAME: &'static str = "Request Signature";
}

impl ServerScheme for Signature {
    type Credential = String;

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

impl ClientScheme for Signature {
    type Secret = String;

    fn write(secret: &String, request: &mut Outgoing<'_>) -> std::result::Result<(), Unsendable> {
        request.header(HeaderName::from_static("x-signature"), secret)
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
