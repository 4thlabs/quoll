quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(header("X-Note") id: i64);
    }
}

fn main() {}
