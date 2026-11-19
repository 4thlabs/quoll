use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/50%25" => list();
    }
}

fn main() {}
