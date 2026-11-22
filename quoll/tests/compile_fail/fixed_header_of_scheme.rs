use quoll::rest;

rest::api! {
    #[openapi(none)]
    #[header("X-Api-Key" = "public")]
    pub api Api = "";

    pub scheme Key = header "X-Api-Key";

    #[auth(Key)]
    pub group Admin {
        GET "/stats" => stats() -> u32;
    }
}

fn main() {}
