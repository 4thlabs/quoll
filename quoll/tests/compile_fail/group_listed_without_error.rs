use std::fmt;

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
    // Its trait is written before the api lists it: its second route cannot take the api's `#[error]`.
    quoll::group! {
        #[openapi(none)]
        pub group Gists {
            #[error(super::HubError)]
            GET "/gists" => list();
            GET "/gists/{id}" => get(id: String) -> String;
        }
    }
}

quoll::api! {
    #[openapi(none)]
    #[error(HubError)]
    pub api Hub = "" { gists::Gists };
}

fn main() {}
