// `#[error]` without the type it names.
quoll::api! {
    pub api Api = "/api";

    #[error]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
