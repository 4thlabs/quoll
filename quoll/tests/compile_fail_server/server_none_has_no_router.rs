quoll::api! {
    /// A third-party API, which the application only calls.
    #[server(none)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: &str, repo: &str) -> String;
    }
}

struct App;

fn main() {
    let _ = GitHub::router(App);
}
