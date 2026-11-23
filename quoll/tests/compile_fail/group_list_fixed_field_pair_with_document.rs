use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, schemars::JsonSchema)]
pub struct Request {
    model: String,
}

mod streams {
    quoll::group! {
        pub group Streams {
            #[body("stream" = true)]
            POST "/messages" => stream(request: super::Request);
        }
    }
}

quoll::api! {
    pub api Api = "/v1" { streams::Streams };

    pub group Messages {
        #[body("stream" = false)]
        POST "/messages" => create(request: Request);
    }
}

fn main() {}
