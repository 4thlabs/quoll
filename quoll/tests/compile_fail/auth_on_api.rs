use quoll::rest;

rest::api! {
    #[auth(Bearer)]
    pub api Api = "/api";
}

fn main() {}
