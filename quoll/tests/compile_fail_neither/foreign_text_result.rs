use quoll::rest;

// A `Text` of the application's own, which the contract reads as quoll's as a result too.
#[derive(serde::Serialize, serde::Deserialize)]
pub struct Text {
    pub body: String,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/readme" => readme() -> Text;
    }
}

fn main() {}
