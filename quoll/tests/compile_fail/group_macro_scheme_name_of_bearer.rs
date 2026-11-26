use http::request::Parts;
use http::HeaderName;
use quoll::auth::{Outgoing, Unsendable};
use quoll::{Result, ClientScheme, Scheme, ServerScheme, openapi::SecurityScheme};

/// A scheme of the application's own, which takes the name of the crate's `Bearer`.
pub struct Mine;

impl Scheme for Mine {
    const NAME: &'static str = "Bearer";
}

impl ServerScheme for Mine {
    type Credential = String;

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Mine")
    }
}

impl ClientScheme for Mine {
    type Secret = String;

    fn write(secret: &String, request: &mut Outgoing<'_>) -> std::result::Result<(), Unsendable> {
        request.header(HeaderName::from_static("x-mine"), secret)
    }
}

quoll::group! {
    // The scheme of the application's first: the pair is checked whatever its order.
    #[auth(crate::Mine | Bearer)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
