use quoll::rest;
use serde::{Deserialize, Serialize};

// A model of a third-party API, which has no schema: a contract that is only called needs none.
#[derive(Debug, Serialize, Deserialize)]
pub struct Repo {
    pub full_name: String,
}

rest::api! {
    /// GitHub's API, which the application only calls.
    #[openapi(none)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: String, repo: String) -> Repo;
    }
}

fn main() {}
