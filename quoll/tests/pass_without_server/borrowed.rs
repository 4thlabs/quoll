use serde::Serialize;

#[derive(Serialize, schemars::JsonSchema)]
pub struct NewNote {
    pub title: String,
}

// A contract with no server takes as before what the server could not receive owned: the client borrows each argument
// as it is written, and the document describes it as it did.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: &mut NewNote) -> String;
        POST "/texts" => text(text: &&str) -> String;
        POST "/files" => file(path: &std::path::Path) -> String;
        GET  "/search?q={q}" => search(q: &'_ str) -> String;
    }
}

mod called {
    use serde::Serialize;

    quoll::api! {
        #[openapi(none)]
        pub api Called = "/api";

        pub group Sent {
            POST "/any" => send(value: &impl Serialize) -> String;
        }
    }
}

fn main() {
    let _ = Api::openapi();
}
