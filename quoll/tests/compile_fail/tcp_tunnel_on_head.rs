quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group Exec {
        HEAD "/exec/{id}/start" => start(id: &str) -> quoll::TcpTunnel;
    }
}

fn main() {}
