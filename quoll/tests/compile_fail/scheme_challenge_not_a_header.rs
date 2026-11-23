use http::HeaderValue;
use http::request::Parts;
use quoll::{Result, Scheme, openapi::SecurityScheme};

/// A scheme of the application's whose challenge is not a header's value.
pub struct Hmac;

impl Scheme for Hmac {
    type Credential = String;

    const NAME: &'static str = "Hmac";
    const CHALLENGE: HeaderValue = HeaderValue::from_static("Hmac\nrealm");

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

quoll::api! {
    pub api Api = "/api";

    #[auth(Hmac)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
