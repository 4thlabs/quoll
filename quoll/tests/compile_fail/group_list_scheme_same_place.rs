mod notes {
    quoll::group! {
        #[auth(Bearer | crate::Login)]
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };

    pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";
}

fn main() {}
