// A scope written twice.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, scopes("repo", "repo"))]
        GET "/notes" => list();
    }
}

fn main() {}
