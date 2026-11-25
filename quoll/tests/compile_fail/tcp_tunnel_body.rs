use quoll::TcpTunnel;

quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group Exec {
        POST "/exec/{id}/start" => start(id: &str, input: TcpTunnel);
    }
}

fn main() {}
