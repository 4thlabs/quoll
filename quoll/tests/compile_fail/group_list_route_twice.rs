mod archive {
    quoll::group! {
        pub group Archive {
            GET "/notes/{id}" => get(id: i64);
        }
    }
}

quoll::api! {
    pub api Api = "/api" { archive::Archive };

    pub group Notes {
        GET "/notes/{note}" => get(note: i64);
    }
}

fn main() {}
