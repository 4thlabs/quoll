use quoll::rest;

#[derive(serde::Serialize, serde::Deserialize)]
pub struct Message {
    pub text: String,
}

/// Not the media type of a JSON body.
const PLAIN: &str = "text/plain; charset=utf-8";

rest::api! {
    #[openapi(none)]
    #[header("Content-Type" = PLAIN)]
    pub api Api = "";

    pub group Messages {
        POST "/messages" => post(message: Message);
    }
}

fn main() {}
