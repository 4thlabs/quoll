use quoll::rest;

// `scopes` written twice.
rest::api! {
    pub api Api = "/api";

    #[auth(Bearer, scopes("repo"), scopes("admin"))]
    pub group Repos {
        GET "/repos" => list();
    }
}

fn main() {}
