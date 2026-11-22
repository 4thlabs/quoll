use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes?tag={first}&tag={second}" => list(first: String, second: String);
    }
}

fn main() {}
