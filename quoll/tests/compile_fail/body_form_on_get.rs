use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Auth {
        GET "/token" => token(login: Form<Login>);
    }
}

fn main() {}
