use http::HeaderValue;
use http::request::Parts;
use http::HeaderName;
use quoll::auth::{Outgoing, Unsendable};
use quoll::{Result, ClientScheme, Scheme, ServerScheme, openapi::SecurityScheme};

/// A scheme of the application's whose challenge is not a header's value.
pub struct Hmac;

impl Scheme for Hmac {
    const NAME: &'static str = "Hmac";
}

impl ServerScheme for Hmac {
    type Credential = String;
    const CHALLENGE: HeaderValue = HeaderValue::from_static("Hmac\nrealm");

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

impl ClientScheme for Hmac {
    type Secret = String;

    fn write(secret: &String, request: &mut Outgoing<'_>) -> std::result::Result<(), Unsendable> {
        request.header(HeaderName::from_static("x-signature"), secret)
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
