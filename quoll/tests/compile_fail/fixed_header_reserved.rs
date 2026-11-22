use quoll::rest;

rest::api! {
    #[openapi(none)]
    #[header("Host" = "api.example.com")]
    pub api Api = "";
}

fn main() {}
