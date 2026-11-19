use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        FETCH "/notes" => list();
    }
}

fn main() {}
