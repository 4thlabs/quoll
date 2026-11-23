quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{self}" => get(id: i64);
    }
}

fn main() {}
