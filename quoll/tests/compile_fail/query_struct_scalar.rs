quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => search(query contains: String);
    }
}

fn main() {}
