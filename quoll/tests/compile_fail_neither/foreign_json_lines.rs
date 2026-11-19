use quoll::rest;

mod lines {
    // A `JsonLines` of another crate's, which the contract reads as quoll's.
    #[derive(serde::Serialize, serde::Deserialize)]
    pub struct JsonLines<T>(pub Vec<T>);
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/lines" => lines() -> lines::JsonLines<String>;
    }
}

fn main() {}
