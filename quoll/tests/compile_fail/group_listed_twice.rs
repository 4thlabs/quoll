mod notes {
    quoll::group! {
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes, notes::Notes };
}

fn main() {}
