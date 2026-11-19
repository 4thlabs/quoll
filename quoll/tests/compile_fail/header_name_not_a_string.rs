use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list(header(X-Page) page: u32);
    }
}

fn main() {}
