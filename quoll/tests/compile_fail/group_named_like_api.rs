use quoll::rest;

rest::api! {
    pub api Notes = "/api";

    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
