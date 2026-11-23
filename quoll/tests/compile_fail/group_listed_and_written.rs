mod notes {
    quoll::group! {
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };

    pub group Notes {
        GET "/others" => list();
    }
}

fn main() {}
