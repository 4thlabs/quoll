use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{tags}" => get(tags: std::collections::HashMap<String, String>);
    }
}

fn main() {}
