mod notes {
    use serde::{Deserialize, Serialize};

    // A `quoll::group!` has a document too, which describes every type its routes name.
    #[derive(Serialize, Deserialize)]
    pub struct Note {
        pub title: String,
    }

    quoll::group! {
        pub group Notes {
            GET "/notes/{id}" => get(id: i64) -> Note;
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
