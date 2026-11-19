use quoll::rest;

rest::api! {
    pub api Api = "/api";

    #[openapi(none)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
