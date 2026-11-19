use quoll::rest;

rest::api! {
    pub api Api = "/api";
    #[auth(Bearer)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
