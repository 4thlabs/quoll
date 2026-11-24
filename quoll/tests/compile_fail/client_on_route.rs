quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[client(none)]
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
