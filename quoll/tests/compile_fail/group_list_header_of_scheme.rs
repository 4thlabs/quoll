use quoll::rest;

mod notes {
    use quoll::rest;

    rest::group! {
        #[auth(crate::Key)]
        pub group Notes {
            GET "/notes" => list(header("X-Api-Key") key: String);
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };

    pub scheme Key = header "X-Api-Key";
}

fn main() {}
