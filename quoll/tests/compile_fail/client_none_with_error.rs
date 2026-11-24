quoll::api! {
    /// A webhook receiver, which the application only serves.
    #[client(none)]
    #[error(String)]
    pub api Hooks = "/hooks";

    pub group Events {
        #[error(u16)]
        POST "/events" => receive(kind: String);
    }
}

fn main() {}
