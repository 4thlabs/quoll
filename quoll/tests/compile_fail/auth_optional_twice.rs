use quoll::rest;

// `optional` written twice.
rest::api! {
    pub api Api = "/api";

    #[auth(Bearer, optional, optional)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
