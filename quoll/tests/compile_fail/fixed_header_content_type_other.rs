#[derive(serde::Serialize, serde::Deserialize)]
pub struct Message {
    pub text: String,
}

quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Messages {
        #[header("Content-Type" = "text/plain")]
        POST "/messages" => post(message: Message);
    }
}

fn main() {}
