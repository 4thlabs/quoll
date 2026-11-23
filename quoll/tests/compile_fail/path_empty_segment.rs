quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes//all" => list();
    }
}

fn main() {}
