use quoll::rest;

// `#[error]` names a type, not a string.
rest::api! {
    #[error("GitHubError")]
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
