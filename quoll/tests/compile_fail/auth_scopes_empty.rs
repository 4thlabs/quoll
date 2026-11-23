// `scopes` with no scope.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, scopes())]
        GET "/notes" => list();
    }
}

fn main() {}
