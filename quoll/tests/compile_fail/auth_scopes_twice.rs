// `scopes` written twice.
quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer, scopes("repo"), scopes("admin"))]
    pub group Repos {
        GET "/repos" => list();
    }
}

fn main() {}
