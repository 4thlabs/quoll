use quoll::rest;

// One of the crate's schemes named through `rest`, which `#[auth(...)]` takes by its name alone.
rest::api! {
    pub api Api = "/api";

    #[auth(rest::Bearer)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
