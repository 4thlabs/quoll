use quoll::rest;

rest::api! {
    #[openapi(all)]
    pub api Api = "/api";
}

fn main() {}
