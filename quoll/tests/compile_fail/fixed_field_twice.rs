use quoll::rest;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, schemars::JsonSchema)]
pub struct Request {
    model: String,
}

rest::api! {
    #[openapi(none)]
    pub api Api = "/v1";

    pub group Messages {
        #[body("stream" = true)]
        #[body("stream" = false)]
        POST "/messages" => create(request: Request);
    }
}

fn main() {}
