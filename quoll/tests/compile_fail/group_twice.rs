quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list();
    }

    pub group Notes {
        GET "/other" => list();
    }
}

fn main() {}
