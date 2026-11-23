// An option `#[auth]` does not take.
quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer, required)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
