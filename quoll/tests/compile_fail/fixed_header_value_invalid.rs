use quoll::rest;

rest::api! {
    #[openapi(none)]
    #[header("X-Version" = "2022-11-28 ")]
    pub api Api = "";
}

fn main() {}
