// A type of quoll's that the contract reads otherwise: a result.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: Created<i64>) -> String;
    }
}

fn main() {}
