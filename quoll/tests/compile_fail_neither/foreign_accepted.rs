use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

// An `Accepted` of the application's own, which the contract reads as quoll's.
pub struct Accepted<T>(pub T);

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Job {
    pub id: u64,
}

quoll::api! {
    pub api Api = "/api";

    pub group Exports {
        POST "/exports" => export(name: String) -> Accepted<Job>;
    }
}

fn main() {}
