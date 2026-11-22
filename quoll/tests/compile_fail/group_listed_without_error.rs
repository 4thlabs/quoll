use std::fmt;

use quoll::rest;
use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct HubError {
    pub message: String,
}

impl fmt::Display for HubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

mod gists {
    use quoll::rest;

    // Its trait is written before the api lists it: it cannot take the api's `#[error]`.
    rest::group! {
        #[openapi(none)]
        pub group Gists {
            GET "/gists" => list();
        }
    }
}

rest::api! {
    #[openapi(none)]
    #[error(HubError)]
    pub api Hub = "" { gists::Gists };
}

fn main() {}
