// `#[error]` names a type, not a string.
quoll::api! {
    #[error("GitHubError")]
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
