quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    #[header("X-Client" = "cli")]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}
