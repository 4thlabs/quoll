quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Messages {
        #[header("Content-Type" = "application/json; charset=utf-8")]
        GET "/messages" => list() -> Vec<String>;
    }
}

fn main() {}
