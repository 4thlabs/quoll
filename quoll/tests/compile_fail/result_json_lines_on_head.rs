use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> rest::JsonLines<String>;
    }
}

fn main() {}
