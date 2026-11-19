use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[deprecated]
        GET "/notes" => list();
    }
}

fn main() {}
