use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, schemars::JsonSchema)]
pub struct Request {
    model: String,
}

const STREAM: bool = true;

quoll::api! {
    #[openapi(none)]
    pub api Api = "/v1";

    pub group Messages {
        #[body("stream" = STREAM)]
        POST "/messages" => create(request: Request);
    }
}

fn main() {}
