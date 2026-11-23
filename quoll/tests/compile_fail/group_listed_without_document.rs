mod notes {
    quoll::group! {
        #[openapi(none)]
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
