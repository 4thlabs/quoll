use quoll::rest;

rest::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64, header("Authorization") token: String);
    }
}

fn main() {}
