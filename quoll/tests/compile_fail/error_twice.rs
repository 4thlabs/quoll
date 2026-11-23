// Two `#[error]` on one route.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[error(String)]
        #[error(String)]
        GET "/notes" => list();
    }
}

fn main() {}
