use http::request::Parts;
use quoll::{Result, Scheme, openapi::SecurityScheme};

/// A scheme written as it was before `Scheme` was split.
pub struct Hmac;

impl Scheme for Hmac {
    type Credential = String;

    const NAME: &'static str = "Hmac";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

fn main() {}
