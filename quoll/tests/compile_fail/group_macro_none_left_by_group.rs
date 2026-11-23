quoll::group! {
    #[openapi(none)]
    #[header(none)]
    pub group Status {
        #[header("X-Version" = none)]
        GET "/status" => status() -> String;
    }
}

fn main() {}
