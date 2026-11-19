use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> Bytes;
    }
}

fn main() {}
