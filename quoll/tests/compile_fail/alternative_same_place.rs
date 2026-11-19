use quoll::rest;

// Two schemes of an alternative that read one place.
rest::api! {
    pub api Api = "/api";

    pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";

    #[auth(Bearer | Login)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
