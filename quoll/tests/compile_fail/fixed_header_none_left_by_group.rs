quoll::api! {
    #[openapi(none)]
    #[header("X-Version" = "1")]
    pub api Api = "";

    #[header(none)]
    pub group Status {
        #[header("X-Version" = none)]
        GET "/status" => status() -> String;
    }
}

fn main() {}
