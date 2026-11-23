quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get();
        GET "/notes?page={page}" => list();
    }
}

fn main() {}
