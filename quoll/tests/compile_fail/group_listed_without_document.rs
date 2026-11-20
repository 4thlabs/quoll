use quoll::rest;

mod notes {
    use quoll::rest;

    rest::group! {
        #[openapi(none)]
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
