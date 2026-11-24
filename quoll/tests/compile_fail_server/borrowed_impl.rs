quoll::api! {
    #[openapi(none)]
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: &impl serde::Serialize);
    }
}

fn main() {}
