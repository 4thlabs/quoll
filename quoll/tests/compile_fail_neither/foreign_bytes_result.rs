use quoll::rest;

// A `Bytes` of the application's own, which the contract reads as quoll's as a result too.
#[derive(serde::Serialize, serde::Deserialize)]
pub struct Bytes(pub Vec<u8>);

rest::api! {
    pub api Api = "/api";

    pub group Files {
        GET "/logo" => logo() -> Bytes;
    }
}

fn main() {}
