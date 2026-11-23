//! The groups of another block an api with an `#[error(E)]` lists: one whose routes each name their shape, and any
//! group under an api whose shape is the default, `quoll::Problem`.

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
    quoll::group! {
        // No `#[error]` of the group's: each route names its own.
        #[openapi(none)]
        pub group Gists {
            #[error(super::HubError)]
            GET "/gists" => list();
            #[error(quoll::Problem)]
            GET "/gists/{id}" => get(id: String) -> String;
        }
    }

    quoll::group! {
        #[openapi(none)]
        pub group Stars {
            GET "/stars" => list();
        }
    }
}

quoll::api! {
    #[openapi(none)]
    #[error(HubError)]
    pub api Hub = "" { gists::Gists };
}

quoll::api! {
    #[openapi(none)]
    #[error(quoll::Problem)]
    pub api Mirror = "/mirror" { gists::Stars };
}

fn main() {}
