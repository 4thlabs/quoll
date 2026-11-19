use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, optional)]
        GET "/notes" => list();
    }
}

fn main() {}
