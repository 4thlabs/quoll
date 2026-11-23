quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id);
    }
}

fn main() {}
