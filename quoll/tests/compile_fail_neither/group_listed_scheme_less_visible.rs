pub mod notes {
    quoll::group! {
        #[auth(crate::Key)]
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };

    pub(crate) scheme Key = header "X-Api-Key";
}

fn main() {}
