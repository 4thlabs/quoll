use quoll::rest;

#[derive(serde::Serialize, serde::Deserialize)]
pub struct Message {
    pub text: String,
}

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Messages {
        #[header("Content-Type" = "text/plain")]
        POST "/messages" => post(message: Message);
    }
}

fn main() {}
