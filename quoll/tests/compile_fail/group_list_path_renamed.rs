use quoll::rest;

mod notes {
    use quoll::rest;

    rest::group! {
        pub group Notes {
            GET "/notes/{id}" => get(id: i64);
        }
    }
}

mod files {
    use quoll::rest;

    rest::group! {
        pub group Files {
            DELETE "/notes/{key}" => remove(key: String);
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes, files::Files };
}

fn main() {}
