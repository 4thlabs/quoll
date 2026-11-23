mod notes {
    quoll::group! {
        pub group Notes {
            GET "/notes/{id}" => get(id: i64);
        }
    }
}

mod files {
    quoll::group! {
        pub group Files {
            DELETE "/notes/{key}" => remove(key: String);
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes, files::Files };
}

fn main() {}
