mod output {
    // A `ByteStream` of another crate's, which the contract reads as quoll's.
    #[derive(serde::Serialize, serde::Deserialize)]
    pub struct ByteStream(pub Vec<u8>);
}

quoll::api! {
    pub api Api = "/api";

    pub group Logs {
        GET "/logs" => logs() -> output::ByteStream;
    }
}

fn main() {}
