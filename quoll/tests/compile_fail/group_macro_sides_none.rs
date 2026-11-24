quoll::group! {
    #[client(none)]
    #[server(none)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

fn main() {}
