const VERSION: u32 = 3;

quoll::api! {
    #[openapi(none)]
    #[header("X-Version" = VERSION)]
    pub api Api = "";
}

fn main() {}
