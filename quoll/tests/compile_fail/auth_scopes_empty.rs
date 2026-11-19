use quoll::rest;

// `scopes` with no scope.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, scopes())]
        GET "/notes" => list();
    }
}

fn main() {}
