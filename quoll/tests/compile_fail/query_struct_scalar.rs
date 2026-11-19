use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => search(query contains: String);
    }
}

fn main() {}
