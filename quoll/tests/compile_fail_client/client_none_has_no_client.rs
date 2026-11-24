quoll::api! {
    /// A webhook receiver, which the application only serves.
    #[client(none)]
    pub api Hooks = "/hooks";

    pub group Events {
        POST "/events" => receive(kind: String);
    }
}

fn main() {
    let _ = Hooks::client("http://localhost:8080");
}
