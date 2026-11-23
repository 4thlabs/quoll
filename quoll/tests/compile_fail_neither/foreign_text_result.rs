use schemars::JsonSchema;

// A `Text` of the application's own, which the contract reads as quoll's as a result too.
#[derive(serde::Serialize, serde::Deserialize, JsonSchema)]
pub struct Text {
    pub body: String,
}

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/readme" => readme() -> Text;
    }
}

fn main() {}
