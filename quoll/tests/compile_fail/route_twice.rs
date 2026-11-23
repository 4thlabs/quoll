quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64);
        GET "/notes/{note}" => get_again(note: i64);
    }
}

fn main() {}
