use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes?page" => list(page: u32);
    }
}

fn main() {}
