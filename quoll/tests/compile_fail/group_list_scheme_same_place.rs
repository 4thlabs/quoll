use quoll::rest;

mod notes {
    use quoll::rest;

    rest::group! {
        #[auth(Bearer | crate::Login)]
        pub group Notes {
            GET "/notes" => list();
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };

    pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";
}

fn main() {}
