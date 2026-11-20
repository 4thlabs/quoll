use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{self, Result, Scheme};

/// A scheme of the application's own, whose name has a space.
pub struct Signed;

impl Scheme for Signed {
    type Credential = String;

    const NAME: &'static str = "Signed request";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

rest::group! {
    #[auth(Signed)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
