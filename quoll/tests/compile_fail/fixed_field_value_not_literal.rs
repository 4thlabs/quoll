use quoll::rest;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, schemars::JsonSchema)]
pub struct Request {
    model: String,
}

const STREAM: bool = true;

rest::api! {
    #[openapi(none)]
    pub api Api = "/v1";

    pub group Messages {
        #[body("stream" = STREAM)]
        POST "/messages" => create(request: Request);
    }
}

fn main() {}
