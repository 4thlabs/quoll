use schemars::JsonSchema;

// A `Text` of the application's own, which the contract reads as quoll's.
#[derive(serde::Serialize, serde::Deserialize, JsonSchema)]
pub struct Text {
    pub body: String,
}

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: Text);
    }
}

fn main() {}
