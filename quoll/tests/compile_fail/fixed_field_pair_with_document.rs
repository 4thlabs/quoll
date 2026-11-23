use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, schemars::JsonSchema)]
pub struct Request {
    model: String,
}

quoll::api! {
    pub api Api = "/v1";

    pub group Messages {
        #[body("stream" = false)]
        POST "/messages" => create(request: Request);

        #[body("stream" = true)]
        POST "/messages" => stream(request: Request);
    }
}

fn main() {}
