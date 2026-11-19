use quoll::rest;

rest::api! {
    pub api Api = "/api";
    #[derive(Debug)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
