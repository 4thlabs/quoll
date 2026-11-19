use quoll::rest;

rest::api! {
    /// A third-party API, which the application only calls.
    #[openapi(none)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: String, repo: String) -> String;
    }
}

fn main() {
    let _ = GitHub::openapi();
}
