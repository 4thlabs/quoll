quoll::api! {
    /// A webhook receiver, which the application only serves.
    #[client(none)]
    pub api Hooks = "/hooks";

    pub group Events {
        POST "/events" => receive(kind: String);
    }
}

struct Fake;

impl Events for Fake {}

fn main() {}
