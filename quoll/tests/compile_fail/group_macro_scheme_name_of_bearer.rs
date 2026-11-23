use http::request::Parts;
use quoll::{Result, Scheme, openapi::SecurityScheme};

/// A scheme of the application's own, which takes the name of the crate's `Bearer`.
pub struct Mine;

impl Scheme for Mine {
    type Credential = String;

    const NAME: &'static str = "Bearer";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Mine")
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
