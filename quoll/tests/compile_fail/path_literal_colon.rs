quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/:id" => get(id: i64);
    }
}

fn main() {}
