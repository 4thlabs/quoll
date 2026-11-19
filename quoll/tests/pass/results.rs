use quoll::rest::{self, Accepted, Bytes, Text};
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
pub struct Job {
    pub id: u64,
}

// The results the contract names by their name are the user's imports, used whatever the features of quoll.
rest::api! {
    pub api Api = "/api";

    pub group Files {
        POST "/exports" => export(name: String) -> Accepted<Job>;
        GET  "/readme"  => readme() -> Text;
        PUT  "/readme"  => write(text: Text) -> Text;
        GET  "/logo"    => logo() -> Bytes;
        GET  "/icon"    => icon() -> rest::Bytes;
        GET  "/jobs"    => jobs() -> rest::Accepted<Vec<Job>>;
    }
}

fn main() {}
