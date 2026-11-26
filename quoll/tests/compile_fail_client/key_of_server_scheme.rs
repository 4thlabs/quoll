use http::request::Parts;
use quoll::{Result, Scheme, ServerScheme, openapi::SecurityScheme};

/// A scheme a server reads, which no client writes.
pub struct Hmac;

impl Scheme for Hmac {
    const NAME: &'static str = "Hmac";
}

impl ServerScheme for Hmac {
    type Credential = String;

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

fn main() {
    let _ = quoll::client::key::<Hmac>(String::from("k-1"));
}
