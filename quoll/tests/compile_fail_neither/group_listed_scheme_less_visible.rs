use quoll::rest;

pub mod notes {
    use quoll::rest;

    rest::group! {
        #[auth(crate::Key)]
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };

    pub(crate) scheme Key = header "X-Api-Key";
}

fn main() {}
