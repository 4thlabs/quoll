use quoll::rest;

const VERSION: u32 = 3;

rest::api! {
    #[openapi(none)]
    #[header("X-Version" = VERSION)]
    pub api Api = "";
}

fn main() {}
