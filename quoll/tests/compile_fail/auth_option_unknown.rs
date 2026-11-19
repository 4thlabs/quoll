use quoll::rest;

// An option `#[auth]` does not take.
rest::api! {
    pub api Api = "/api";

    #[auth(Bearer, required)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
