use quoll::TcpTunnel;

quoll::group! {
    // A group of both sides, which an api that serves may list.
    pub group Exec {
        POST "/exec/{id}/start" => start(id: &str) -> TcpTunnel;
    }
}

fn main() {}
