use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/token" => token(login: Form<Login>);
    }
}

fn main() {}
