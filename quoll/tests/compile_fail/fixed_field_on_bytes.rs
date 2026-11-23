use quoll::rest;

rest::api! {
    #[openapi(none)]
    pub api Api = "/v1";

    pub group Messages {
        #[body("stream" = true)]
        POST "/messages" => create(bytes: rest::Bytes);
    }
}

fn main() {}
