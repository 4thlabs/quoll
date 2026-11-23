use quoll::{Accepted, Bytes, Text};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Job {
    pub id: u64,
}

// The results the contract names by their name are the user's imports, used whatever the features of quoll.
quoll::api! {
    pub api Api = "/api";

    pub group Files {
        POST "/exports" => export(name: String) -> Accepted<Job>;
        GET  "/readme"  => readme() -> Text;
        PUT  "/readme"  => write(text: Text) -> Text;
        GET  "/logo"    => logo() -> Bytes;
        GET  "/icon"    => icon() -> quoll::Bytes;
        GET  "/jobs"    => jobs() -> quoll::Accepted<Vec<Job>>;
    }
}

fn main() {}
