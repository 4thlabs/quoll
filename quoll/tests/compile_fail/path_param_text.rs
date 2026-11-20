use quoll::rest;

// A type of quoll's that the contract reads otherwise: a body or a result.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: Text) -> String;
    }
}

fn main() {}
