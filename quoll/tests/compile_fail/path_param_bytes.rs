use quoll::rest;

// A type of quoll's that the contract reads otherwise: a body or a result.
rest::api! {
    pub api Api = "/api";

    pub group Files {
        GET "/files/{id}" => get(id: Bytes) -> String;
    }
}

fn main() {}
