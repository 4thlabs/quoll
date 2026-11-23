quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}?id={id}" => get(id: i64);
    }
}

fn main() {}
