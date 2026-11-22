use quoll::rest;

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    #[header(none)]
    #[header(none)]
    pub group Health {
        GET "/health" => check();
    }
}

fn main() {}
