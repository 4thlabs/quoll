use quoll::rest;

// A `Vec`, which is not a struct.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => search(query tags: Vec<String>);
    }
}

fn main() {}
