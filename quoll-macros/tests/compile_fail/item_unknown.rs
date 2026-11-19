use quoll::rest;

rest::api! {
    pub scheme Key = header "X-Api-Key";
    pub api Api = "/api";
}

fn main() {}
