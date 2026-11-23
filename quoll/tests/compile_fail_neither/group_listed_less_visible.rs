mod notes {
    quoll::group! {
        pub(crate) group Notes {
            GET "/notes" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
