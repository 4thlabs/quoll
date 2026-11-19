use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[openapi(none)]
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
