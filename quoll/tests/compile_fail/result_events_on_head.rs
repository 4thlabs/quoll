use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> Events<String>;
    }
}

fn main() {}
