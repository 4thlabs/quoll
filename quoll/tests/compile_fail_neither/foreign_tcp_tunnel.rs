mod tunnel {
    // A `TcpTunnel` of another crate's, which the contract reads as quoll's.
    #[derive(serde::Serialize, serde::Deserialize)]
    pub struct TcpTunnel;
}

quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group Exec {
        POST "/exec/{id}/start" => start(id: &str) -> tunnel::TcpTunnel;
    }
}

fn main() {}
