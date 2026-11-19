use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{ids}" => get(ids: Vec<i64>);
    }
}

fn main() {}
