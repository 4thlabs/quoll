use quoll::rest;

// A scheme named as an item the macro writes: the client of the api.
rest::api! {
    pub api Api = "/api";

    pub scheme ApiClient = header "X-Api-Key";

    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
