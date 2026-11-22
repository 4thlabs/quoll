use quoll::rest;

rest::api! {
    #[openapi(none)]
    #[header(Accept = "application/json")]
    pub api Api = "";
}

fn main() {}
