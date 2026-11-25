use quoll::{Created, TcpTunnel};

quoll::api! {
    #[server(none)]
    pub api Docker = "/v1.47";

    pub group Exec {
        POST "/exec/{id}/start" => start(id: &str) -> Created<TcpTunnel>;
    }
}

fn main() {}
