use quoll::rest;

// An `Events` of the application's own, which the contract reads as quoll's.
#[derive(serde::Serialize, serde::Deserialize)]
pub struct Events<T> {
    pub items: Vec<T>,
}

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/events" => events() -> Events<String>;
    }
}

fn main() {}
