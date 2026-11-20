use quoll::rest;

// A type of quoll's that the contract reads otherwise: a body.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: Form<Login>) -> String;
    }
}

fn main() {}
