mod notes {
    quoll::group! {
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { Notes };
}

fn main() {}
