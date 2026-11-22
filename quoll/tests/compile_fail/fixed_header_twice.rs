use quoll::rest;

rest::api! {
    #[openapi(none)]
    #[header("X-Version" = "1")]
    #[header("x-version" = "2")]
    pub api Api = "";
}

fn main() {}
