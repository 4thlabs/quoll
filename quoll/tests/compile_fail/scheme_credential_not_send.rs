use std::rc::Rc;

use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{Result, Scheme};

/// A scheme whose credential cannot cross an `await` of a server on several threads.
pub struct Hmac;

impl Scheme for Hmac {
    type Credential = Rc<String>;

    const NAME: &'static str = "Hmac";

    fn read(_request: &Parts) -> Result<Option<Rc<String>>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

fn main() {}
