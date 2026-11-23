// One of the crate's schemes named through `quoll`, which `#[auth(...)]` takes by its name alone.
quoll::api! {
    pub api Api = "/api";

    #[auth(quoll::Bearer)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
