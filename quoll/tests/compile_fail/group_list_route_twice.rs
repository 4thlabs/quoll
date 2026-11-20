use quoll::rest;

mod archive {
    use quoll::rest;

    rest::group! {
        pub group Archive {
            GET "/notes/{id}" => get(id: i64);
        }
    }
}

rest::api! {
    pub api Api = "/api" { archive::Archive };

    pub group Notes {
        GET "/notes/{note}" => get(note: i64);
    }
}

fn main() {}
