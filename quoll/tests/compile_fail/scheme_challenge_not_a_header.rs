use http::HeaderValue;
use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{self, Result, Scheme};

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

rest::api! {
    pub api Api = "/api";

    #[auth(Hmac)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
