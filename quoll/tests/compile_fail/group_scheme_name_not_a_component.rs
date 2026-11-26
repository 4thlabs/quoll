use http::request::Parts;
use http::HeaderName;
use quoll::auth::{Outgoing, Unsendable};
use quoll::{Result, ClientScheme, Scheme, ServerScheme, openapi::SecurityScheme};

/// A scheme of the application's own, whose name has a space.
pub struct Signed;

impl Scheme for Signed {
    const NAME: &'static str = "Signed request";
}

impl ServerScheme for Signed {
    type Credential = String;

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

impl ClientScheme for Signed {
    type Secret = String;

    fn write(secret: &String, request: &mut Outgoing<'_>) -> std::result::Result<(), Unsendable> {
        request.header(HeaderName::from_static("x-signature"), secret)
    }
}

quoll::group! {
    #[auth(Signed)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
