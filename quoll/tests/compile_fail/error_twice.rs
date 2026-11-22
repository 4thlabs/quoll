use quoll::rest;

// Two `#[error]` on one route.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[error(String)]
        #[error(String)]
        GET "/notes" => list();
    }
}

fn main() {}
