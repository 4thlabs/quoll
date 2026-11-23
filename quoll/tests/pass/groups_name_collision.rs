use http::request::Parts;
use quoll::{Result, Scheme, openapi::SecurityScheme};

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
    quoll::group! {
        #[openapi(none)]
        #[auth(crate::Impostor)]
        pub group Notes {
            GET "/notes" => list(header("X-Api-Key") key: String);
        }
    }
}

quoll::api! {
    #[openapi(none)]
    pub api Api = "/api" { notes::Notes };

    pub(crate) scheme Key = header "X-Api-Key";
}

fn main() {
    let _api = Api;
}
