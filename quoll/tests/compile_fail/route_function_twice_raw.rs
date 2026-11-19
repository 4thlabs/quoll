use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => get();
        GET "/archive" => r#get();
    }
}

fn main() {}
