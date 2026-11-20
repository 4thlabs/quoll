use quoll::rest;

// A type of quoll's that the contract reads otherwise: a result.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: Created<i64>) -> String;
    }
}

fn main() {}
