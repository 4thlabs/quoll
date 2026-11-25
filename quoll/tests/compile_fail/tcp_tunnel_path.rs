use quoll::TcpTunnel;

quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group Exec {
        GET "/exec/{tunnel}/json" => inspect(tunnel: TcpTunnel);
    }
}

fn main() {}
