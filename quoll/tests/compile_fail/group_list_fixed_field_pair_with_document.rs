use quoll::rest;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, schemars::JsonSchema)]
pub struct Request {
    model: String,
}

mod streams {
    use quoll::rest;

    rest::group! {
        pub group Streams {
            #[body("stream" = true)]
            POST "/messages" => stream(request: super::Request);
        }
    }
}

rest::api! {
    pub api Api = "/v1" { streams::Streams };

    pub group Messages {
        #[body("stream" = false)]
        POST "/messages" => create(request: Request);
    }
}

fn main() {}
