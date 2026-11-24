quoll::api! {
    pub api Api = "/api";

    #[server(none)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
