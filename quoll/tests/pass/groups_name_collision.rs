use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{self, Result, Scheme};

/// A scheme of the application's own, which reads the header of the api's `Key` under its name: not the api's
/// declared scheme, whose visibility and place the api's checks do not give it.
pub struct Impostor;

impl Scheme for Impostor {
    type Credential = String;

    const NAME: &'static str = "Key";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Impostor")
    }
}

pub mod notes {
    use quoll::rest;

    rest::group! {
        #[openapi(none)]
        #[auth(crate::Impostor)]
        pub group Notes {
            GET "/notes" => list(header("X-Api-Key") key: String);
        }
    }
}

rest::api! {
    #[openapi(none)]
    pub api Api = "/api" { notes::Notes };

    pub(crate) scheme Key = header "X-Api-Key";
}

fn main() {
    let _api = Api;
}
