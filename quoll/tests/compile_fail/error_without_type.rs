use quoll::rest;

// `#[error]` without the type it names.
rest::api! {
    pub api Api = "/api";

    #[error]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
