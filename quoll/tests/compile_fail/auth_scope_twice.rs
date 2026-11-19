use quoll::rest;

// A scope written twice.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, scopes("repo", "repo"))]
        GET "/notes" => list();
    }
}

fn main() {}
