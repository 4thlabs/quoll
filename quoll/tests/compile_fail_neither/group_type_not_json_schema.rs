use quoll::rest;

mod notes {
    use quoll::rest;
    use serde::{Deserialize, Serialize};

    // A `rest::group!` has a document too, which describes every type its routes name.
    #[derive(Serialize, Deserialize)]
    pub struct Note {
        pub title: String,
    }

    rest::group! {
        pub group Notes {
            GET "/notes/{id}" => get(id: i64) -> Note;
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
