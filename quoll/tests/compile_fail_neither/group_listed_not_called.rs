mod notes {
    quoll::group! {
        #[client(none)]
        pub group Notes {
            GET "/notes/{id}" => get(id: i64) -> String;
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
