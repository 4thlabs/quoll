quoll::api! {
    // Only served: a tunnel would have its server side alone.
    #[client(none)]
    pub api Docker = "/v1.47";

    pub group Exec {
        POST "/exec/{id}/start" => start(id: &str) -> quoll::TcpTunnel;
    }
}

fn main() {}
