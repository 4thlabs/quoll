use quoll::rest;

rest::api! {
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
