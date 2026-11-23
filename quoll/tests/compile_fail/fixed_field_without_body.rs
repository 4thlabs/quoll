use quoll::rest;

rest::api! {
    #[openapi(none)]
    pub api Api = "/v1";

    pub group Messages {
        #[body("stream" = true)]
        GET "/messages" => list();
    }
}

fn main() {}
