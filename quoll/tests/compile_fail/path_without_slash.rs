use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "notes" => list();
    }
}

fn main() {}
