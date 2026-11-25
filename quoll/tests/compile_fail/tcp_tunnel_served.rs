use quoll::TcpTunnel;

quoll::api! {
    // An api with a server side, which `#[server(none)]` would leave out.
    pub api Docker = "/v1.47";

    pub group Exec {
        POST "/exec/{id}/start" => start(id: &str) -> TcpTunnel;
    }
}

fn main() {}
