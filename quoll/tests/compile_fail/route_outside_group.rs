use quoll::rest;

rest::api! {
    pub api Api = "/api";
    GET "/notes" => list();
}

fn main() {}
