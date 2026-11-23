use http::request::Parts;
use quoll::{Result, Scheme, openapi::SecurityScheme};

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

quoll::group! {
    #[auth(Signed)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
