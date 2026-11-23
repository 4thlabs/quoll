mod notes {
    quoll::group! {
        #[auth(crate::Key)]
        pub group Notes {
            GET "/notes" => list(header("X-Api-Key") key: String);
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };

    pub scheme Key = header "X-Api-Key";
}

fn main() {}
