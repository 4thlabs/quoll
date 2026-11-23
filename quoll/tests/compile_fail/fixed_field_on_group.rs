use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, schemars::JsonSchema)]
pub struct Request {
    model: String,
}

quoll::api! {
    #[openapi(none)]
    pub api Api = "/v1";

    #[body("stream" = true)]
    pub group Messages {
        POST "/messages" => create(request: Request);
    }
}

fn main() {}
