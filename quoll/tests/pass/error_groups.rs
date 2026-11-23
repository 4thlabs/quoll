//! The groups of another block an api with an `#[error(E)]` lists: one whose routes each name their shape, and any
//! group under an api whose shape is the default, `rest::Problem`.

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

    rest::group! {
        // No `#[error]` of the group's: each route names its own.
        #[openapi(none)]
        pub group Gists {
            #[error(super::HubError)]
            GET "/gists" => list();
            #[error(rest::Problem)]
            GET "/gists/{id}" => get(id: String) -> String;
        }
    }

    rest::group! {
        #[openapi(none)]
        pub group Stars {
            GET "/stars" => list();
        }
    }
}

rest::api! {
    #[openapi(none)]
    #[error(HubError)]
    pub api Hub = "" { gists::Gists };
}

rest::api! {
    #[openapi(none)]
    #[error(rest::Problem)]
    pub api Mirror = "/mirror" { gists::Stars };
}

fn main() {}
