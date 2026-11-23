quoll::api! {
    pub api Notes = "/api";

    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
