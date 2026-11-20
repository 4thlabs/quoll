use quoll::rest;

mod notes {
    use quoll::rest;

    rest::group! {
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };

    pub group Notes {
        GET "/others" => list();
    }
}

fn main() {}
